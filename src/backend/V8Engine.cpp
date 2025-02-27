#include "backend/V8Engine.hpp"

#include "backend/Interpreter.hpp"
#include "backend/WasmOperator.hpp"
#include "backend/WasmUtil.hpp"
#include "mutable/util/macro.hpp"
#include "storage/Store.hpp"
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <fstream>
#include <libplatform/libplatform.h>
#include <mutable/catalog/Catalog.hpp>
#include <mutable/IR/PhysicalOptimizer.hpp>
#include <mutable/IR/Tuple.hpp>
#include <mutable/Options.hpp>
#include <mutable/storage/DataLayoutFactory.hpp>
#include <mutable/storage/Store.hpp>
#include <mutable/util/DotTool.hpp>
#include <mutable/util/enum_ops.hpp>
#include <mutable/util/memory.hpp>
#include <mutable/util/Timer.hpp>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

// must be included after Binaryen due to conflicts, e.g. with `::wasm::Throw`
#include "backend/WasmMacro.hpp"


using namespace m;
using namespace m::storage;
using namespace m::wasm;
using namespace m::wasm::detail;
using args_t = v8::Local<v8::Value>[];


namespace {

namespace options {

/** The Wasm optimization level. */
int wasm_optimization_level = 0;
/** Whether to execute Wasm adaptively. */
bool wasm_adaptive = false;
/** Whether compilation cache should be enabled. */
bool wasm_compilation_cache = true;
/** Whether to dump the generated WebAssembly code. */
bool wasm_dump = false;
/** Whether to dump the generated assembly code. */
bool asm_dump = false;
/** The port to use for the Chrome DevTools web socket. */
uint16_t cdt_port = 0;

}


/*======================================================================================================================
 * V8Engine
 *====================================================================================================================*/

/** The `V8Engine` is a `WasmEngine` using [V8, Google's open source high-performance JavaScript and WebAssembly
 * engine] (https://v8.dev/). */
struct V8Engine : m::WasmEngine
{
    friend void create_V8Engine();
    friend void destroy_V8Engine();
    friend void register_WasmV8();

    private:
    static inline v8::Platform *PLATFORM_ = nullptr;
    v8::ArrayBuffer::Allocator *allocator_ = nullptr;
    v8::Isolate *isolate_ = nullptr;

    /*----- Objects for remote debugging via CDT. --------------------------------------------------------------------*/
    std::unique_ptr<V8InspectorClientImpl> inspector_;

    public:
    V8Engine();
    V8Engine(const V8Engine&) = delete;
    V8Engine(V8Engine&&) = default;

    ~V8Engine();

    static v8::Platform * platform() {
        M_insist(bool(PLATFORM_));
        return PLATFORM_;
    }

    void initialize();
    void compile(const m::MatchBase &plan) const override;
    void execute(const m::MatchBase &plan) override;
};


/*======================================================================================================================
 * Implementation of V8Inspector and helper classes / methods
 *====================================================================================================================*/

inline v8::Local<v8::String> to_v8_string(v8::Isolate *isolate, std::string_view sv) {
    M_insist(isolate);
    return v8::String::NewFromUtf8(isolate, sv.data(), v8::NewStringType::kNormal, sv.length()).ToLocalChecked();
}

inline std::string to_std_string(v8::Isolate *isolate, v8::Local<v8::Value> val) {
    v8::String::Utf8Value utf8(isolate, val);
    return *utf8;
}

inline v8::Local<v8::Object> parse_json(v8::Isolate *isolate, std::string_view json) {
    M_insist(isolate);
    auto Ctx = isolate->GetCurrentContext();
    auto value = v8::JSON::Parse(Ctx, to_v8_string(isolate, json)).ToLocalChecked();
    if (value.IsEmpty())
        return v8::Local<v8::Object>();
    return value->ToObject(Ctx).ToLocalChecked();
}

inline v8_inspector::StringView make_string_view(const std::string &str) {
    return v8_inspector::StringView(reinterpret_cast<const uint8_t*>(str.data()), str.length());
}

inline std::string to_std_string(v8::Isolate *isolate, const v8_inspector::StringView sv) {
    int length = static_cast<int>(sv.length());
    v8::Local<v8::String> message = (
        sv.is8Bit()
        ? v8::String::NewFromOneByte(isolate, reinterpret_cast<const uint8_t*>(sv.characters8()), v8::NewStringType::kNormal, length)
        : v8::String::NewFromTwoByte(isolate, reinterpret_cast<const uint16_t*>(sv.characters16()), v8::NewStringType::kNormal, length)
    ).ToLocalChecked();
    v8::String::Utf8Value result(isolate, message);
    return std::string(*result, result.length());
}

}

void WebSocketChannel::sendResponse(int, std::unique_ptr<v8_inspector::StringBuffer> message)
{
    v8::HandleScope handle_scope(isolate_);
    auto str = to_std_string(isolate_, message->string());
    conn_.send(str);
}

void WebSocketChannel::sendNotification(std::unique_ptr<v8_inspector::StringBuffer> message)
{
    v8::HandleScope handle_scope(isolate_);
    auto str = to_std_string(isolate_, message->string());
    conn_.send(str);
}

V8InspectorClientImpl::V8InspectorClientImpl(int16_t port, v8::Isolate *isolate)
    : isolate_(M_notnull(isolate))
    , server_(port, std::bind(&V8InspectorClientImpl::on_message, this, std::placeholders::_1))
{
    std::cout << "Initiating the V8 inspector server.  To attach to the inspector, open Chrome/Chromium and "
                 "visit\n\n\t"
                 "devtools://devtools/bundled/inspector.html?experiments=true&v8only=true&ws=127.0.0.1:"
              << port << '\n' << std::endl;

    inspector_ = v8_inspector::V8Inspector::create(isolate, this);
    conn_ = std::make_unique<WebSocketServer::Connection>(server_.await());
    channel_ = std::make_unique<WebSocketChannel>(isolate_, *conn_);

    /* Create a debugging session by connecting the V8Inspector instance to the channel. */
    std::string state("mutable");
    session_ = inspector_->connect(
        /* contextGroupId= */ 1,
        /* channel=        */ channel_.get(),
        /* state=          */ make_string_view(state),
        /* trustLevel=     */ v8_inspector::V8Inspector::kFullyTrusted,
        /* pauseState=     */ v8_inspector::V8Inspector::kWaitingForDebugger
    );
}

void V8InspectorClientImpl::register_context(v8::Local<v8::Context> context)
{
    std::string ctx_name("query");
    inspector_->contextCreated(v8_inspector::V8ContextInfo( context, 1, make_string_view(ctx_name)));
}

void V8InspectorClientImpl::deregister_context(v8::Local<v8::Context> context)
{
    inspector_->contextDestroyed(context);
}

void V8InspectorClientImpl::on_message(std::string_view sv)
{
    v8_inspector::StringView msg(reinterpret_cast<const uint8_t*>(sv.data()), sv.length());

    auto Ctx = isolate_->GetCurrentContext();
    v8::HandleScope handle_scope(isolate_);
    auto obj = parse_json(isolate_, sv);

    session_->dispatchProtocolMessage(msg);

    if (not obj.IsEmpty()) {
        auto method = obj->Get(Ctx, to_v8_string(isolate_, "method")).ToLocalChecked();
        auto method_name = to_std_string(isolate_, method);

        if (method_name == "Runtime.runIfWaitingForDebugger") {
            std::string reason("CDT");
            session_->schedulePauseOnNextStatement(make_string_view(reason),
                                                   make_string_view(reason));
            waitFrontendMessageOnPause();
            code_(); // execute the code to debug
        }
    }
}

void V8InspectorClientImpl::runMessageLoopOnPause(int)
{
    static bool is_nested = false;
    if (is_nested) return;

    is_terminated_ = false;
    is_nested = true;
    while (not is_terminated_ and conn_->wait_on_message())
        while (v8::platform::PumpMessageLoop(V8Engine::platform(), isolate_)) { }
    is_terminated_ = true;
    is_nested = false;
}


/*======================================================================================================================
 * V8 Callback Functions
 *
 * Functions to be called from the WebAssembly module to give control flow and pass data to the host.
 *====================================================================================================================*/

void m::wasm::detail::insist(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    M_insist(info.Length() == 1);
    auto idx = info[0].As<v8::BigInt>()->Uint64Value();
    auto [filename, line, msg] = Module::Get().get_message(idx);

    std::cout.flush();
    std::cerr << filename << ':' << line << ": Wasm_insist failed.";
    if (msg)
        std::cerr << "  " << msg << '.';
    std::cerr << std::endl;

    abort();
}

void m::wasm::detail::_throw(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    M_insist(info.Length() == 2);
    auto type = static_cast<m::wasm::exception::exception_t>(info[0].As<v8::BigInt>()->Uint64Value());
    auto idx = info[1].As<v8::BigInt>()->Uint64Value();
    auto [filename, line, msg] = Module::Get().get_message(idx);

    std::ostringstream oss;
    oss << filename << ':' << line << ": Exception `" << m::wasm::exception::names_[type] << "` thrown.";
    if (*msg)
        oss << "  " << msg << '.';
    oss << std::endl;

    throw m::wasm::exception(type, oss.str());
}

void m::wasm::detail::print(const v8::FunctionCallbackInfo<v8::Value> &info)
{
#ifndef NDEBUG
    std::cout << "v8 function callback: ";
#endif
    for (int i = 0; i != info.Length(); ++i) {
        v8::HandleScope handle_scope(info.GetIsolate());
        if (i != 0) std::cout << ',';
        v8::Local<v8::Value> v = info[i];
        if (v->IsInt32())
            std::cout << "0x" << std::hex << uint32_t(v.As<v8::Int32>()->Value()) << std::dec;
        else
            std::cout << *v8::String::Utf8Value(info.GetIsolate(), v);
    }
    std::cout << std::endl;
}

void m::wasm::detail::print_memory_consumption(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    M_insist(Options::Get().statistics);

    auto alloc_total_mem = info[0].As<v8::Uint32>()->Value();
    auto alloc_peak_mem = info[1].As<v8::Uint32>()->Value();

    std::cout << "Allocated memory overall consumption: " << alloc_total_mem / (1024.0 * 1024.0) << " MiB"<< std::endl;
    std::cout << "Allocated memory peak consumption: " << alloc_peak_mem / (1024.0 * 1024.0) << " MiB"<< std::endl;
}

void m::wasm::detail::set_wasm_instance_raw_memory(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    v8::Local<v8::WasmModuleObject> wasm_instance = info[0].As<v8::WasmModuleObject>();
    v8::Local<v8::Int32> wasm_context_id = info[1].As<v8::Int32>();

    auto &wasm_context = WasmEngine::Get_Wasm_Context_By_ID(wasm_context_id->Value());
#ifndef NDEBUG
    std::cerr << "Setting Wasm instance raw memory of the given instance to the VM of Wasm context "
              << wasm_context_id->Value() << " at " << wasm_context.vm.addr() << " of " << wasm_context.vm.size()
              << " bytes" << std::endl;
#endif
    v8::SetWasmInstanceRawMemory(wasm_instance, wasm_context.vm.as<uint8_t*>(), wasm_context.vm.size());
}

void m::wasm::detail::read_result_set(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    auto &context = WasmEngine::Get_Wasm_Context_By_ID(Module::ID());

    auto &root_op = context.plan.get_matched_root();
    auto &schema = root_op.schema();
    auto deduplicated_schema = schema.deduplicate();
    auto deduplicated_schema_without_constants = deduplicated_schema.drop_constants();

    /* Get number of result tuples. */
    auto num_tuples = info[1].As<v8::Uint32>()->Value();
    if (num_tuples == 0)
        return;

    /* Compute address of result set. */
    M_insist(info.Length() == 2);
    auto result_set_offset = info[0].As<v8::Uint32>()->Value();
    M_insist((result_set_offset == 0) == (deduplicated_schema_without_constants.num_entries() == 0),
             "result set offset equals 0 (i.e. nullptr) iff schema contains only constants");
    auto result_set = context.vm.as<uint8_t*>() + result_set_offset;

    /* Find the projection nearest to the plan's root since it will determine the constants omitted in the result set. */
    auto find_projection = [](const Operator &op) -> const ProjectionOperator * {
        auto find_projection_impl = [](const Operator &op, auto &find_projection_ref) -> const ProjectionOperator * {
            if (auto projection_op = cast<const ProjectionOperator>(&op)) {
                return projection_op;
            } else if (auto c = cast<const Consumer>(&op)) {
                M_insist(c->children().size() == 1,
                         "at least one projection without siblings in the operator tree must be contained");
                M_insist(c->schema().num_entries() == c->child(0)->schema().num_entries(),
                         "at least one projection with the same schema as the plan's root must be contained");
#ifndef NDEBUG
                for (std::size_t i = 0; i < c->schema().num_entries(); ++i)
                    M_insist(c->schema()[i].id == c->child(0)->schema()[i].id,
                             "at least one projection with the same schema as the plan's root must be contained");
#endif
                return find_projection_ref(*c->child(0), find_projection_ref);
            } else {
                return nullptr; // no projection found
            }
        };
        return find_projection_impl(op, find_projection_impl);
    };
    auto projection = find_projection(root_op);

    ///> helper function to print given `ast::Constant` \p c of `Type` \p type to \p out
    auto print_constant = [](std::ostringstream &out, const ast::Constant &c, const Type *type){
        if (type->is_none()) {
            out << "NULL";
            return;
        }

        /* Interpret constant. */
        auto value = Interpreter::eval(c);

        visit(overloaded {
            [&](const Boolean&) { out << (value.as_b() ? "TRUE" : "FALSE"); },
            [&](const Numeric &n) {
                switch (n.kind) {
                    case Numeric::N_Int:
                    case Numeric::N_Decimal:
                        out << value.as_i();
                        break;
                    case Numeric::N_Float:
                        if (n.size() <= 32) {
                            const auto old_precision = out.precision(std::numeric_limits<float>::max_digits10 - 1);
                            out << value.as_f();
                            out.precision(old_precision);
                        } else {
                            const auto old_precision = out.precision(std::numeric_limits<double>::max_digits10 - 1);
                            out << value.as_d();
                            out.precision(old_precision);
                        }
                }
            },
            [&](const CharacterSequence&) { out << '"' << reinterpret_cast<char*>(value.as_p()) << '"'; },
            [&](const Date&) {
                const int32_t date = value.as_i(); // signed because year is signed
                const auto oldfill = out.fill('0');
                const auto oldfmt = out.flags();
                out << std::internal
                    << std::setw(date >> 9 > 0 ? 4 : 5) << (date >> 9) << '-'
                    << std::setw(2) << ((date >> 5) & 0xF) << '-'
                    << std::setw(2) << (date & 0x1F);
                out.fill(oldfill);
                out.flags(oldfmt);
            },
            [&](const DateTime&) {
                const time_t time = value.as_i();
                std::tm tm;
                gmtime_r(&time, &tm);
                out << put_tm(tm);
            },
            [](const NoneType&) { M_unreachable("should've been handled earlier"); },
            [](auto&&) { M_unreachable("invalid type"); },
        }, *type);
    };

    if (deduplicated_schema_without_constants.num_entries() == 0) {
        /* Schema contains only constants. Create simple loop to generate `num_tuples` constant result tuples. */
        M_insist(bool(projection), "projection must be found");
        auto &projections = projection->projections();
        if (auto callback_op = cast<const CallbackOperator>(&root_op)) {
            Tuple tup(schema); // tuple entries which are not set are implicitly NULL
            for (std::size_t i = 0; i < schema.num_entries(); ++i) {
                auto &e = schema[i];
                if (e.type->is_none()) continue; // NULL constant
                M_insist(e.id.is_constant());
                tup.set(i, Interpreter::eval(as<const ast::Constant>(projections[i].first)));
            }
            for (std::size_t i = 0; i < num_tuples; ++i)
                callback_op->callback()(schema, tup);
        } else if (auto print_op = cast<const PrintOperator>(&root_op)) {
            std::ostringstream tup;
            for (std::size_t i = 0; i < schema.num_entries(); ++i) {
                auto &e = schema[i];
                if (i)
                    tup << ',';
                M_insist(e.id.is_constant());
                print_constant(tup, as<const ast::Constant>(projections[i].first), e.type);
            }
            for (std::size_t i = 0; i < num_tuples; ++i)
                print_op->out << tup.str() << '\n';
        }
        return;
    }

    /* Create data layout (without constants and duplicates). */
    M_insist(bool(context.result_set_factory), "result set factory must be set");
    auto layout = context.result_set_factory->make(deduplicated_schema_without_constants);

    /* Extract results. */
    if (auto callback_op = cast<const CallbackOperator>(&root_op)) {
        auto loader = Interpreter::compile_load(deduplicated_schema_without_constants, result_set, layout,
                                                deduplicated_schema_without_constants);
        if (schema.num_entries() == deduplicated_schema.num_entries()) {
            /* No deduplication was performed. Compute `Tuple` with constants. */
            M_insist(schema == deduplicated_schema);
            Tuple tup(schema); // tuple entries which are not set are implicitly NULL
            for (std::size_t i = 0; i < schema.num_entries(); ++i) {
                auto &e = schema[i];
                if (e.type->is_none()) continue; // NULL constant
                if (e.id.is_constant()) { // other constant
                    M_insist(bool(projection), "projection must be found");
                    tup.set(i, Interpreter::eval(as<const ast::Constant>(projection->projections()[i].first)));
                }
            }
            Tuple *args[] = { &tup };
            for (std::size_t i = 0; i != num_tuples; ++i) {
                loader(args);
                callback_op->callback()(schema, tup);
                tup.clear();
            }
        } else {
            /* Deduplication was performed. Compute a `Tuple` with duplicates and constants. */
            Tuple tup_dedupl(deduplicated_schema_without_constants);
            Tuple tup_dupl(schema); // tuple entries which are not set are implicitly NULL
            for (std::size_t i = 0; i < schema.num_entries(); ++i) {
                auto &e = schema[i];
                if (e.type->is_none()) continue; // NULL constant
                if (e.id.is_constant()) { // other constant
                    M_insist(bool(projection), "projection must be found");
                    tup_dupl.set(i, Interpreter::eval(as<const ast::Constant>(projection->projections()[i].first)));
                }
            }
            Tuple *args[] = { &tup_dedupl, &tup_dupl };
            for (std::size_t i = 0; i != deduplicated_schema_without_constants.num_entries(); ++i) {
                auto &entry = deduplicated_schema_without_constants[i];
                if (not entry.type->is_none())
                    loader.emit_Ld_Tup(0, i);
                for (std::size_t j = 0; j != schema.num_entries(); ++j) {
                    auto &e = schema[j];
                    if (e.id == entry.id) {
                        M_insist(e.type == entry.type);
                        loader.emit_St_Tup(1, j, e.type);
                    }
                }
                if (not entry.type->is_none())
                    loader.emit_Pop();
            }
            for (std::size_t i = 0; i != num_tuples; ++i) {
                loader(args);
                callback_op->callback()(schema, tup_dupl);
            }
        }
    } else if (auto print_op = cast<const PrintOperator>(&root_op)) {
        /* Compute a `Tuple` with duplicates and constants. */
        Tuple tup(deduplicated_schema_without_constants);
        Tuple *args[] = { &tup };
        auto printer = Interpreter::compile_load(deduplicated_schema_without_constants, result_set, layout,
                                                 deduplicated_schema_without_constants);
        auto ostream_index = printer.add(&print_op->out);
        bool constant_emitted = false;
        std::size_t old_idx = -1UL;
        for (std::size_t i = 0; i != schema.num_entries(); ++i) {
            if (i != 0)
                printer.emit_Putc(ostream_index, ',');
            auto &e = schema[i];
            if (not e.type->is_none()) {
                if (e.id.is_constant()) { // constant except NULL
                    M_insist(bool(projection), "projection must be found");
                    printer.add_and_emit_load(Interpreter::eval(as<const ast::Constant>(projection->projections()[i].first)));
                    constant_emitted = true;
                } else { // actual value
                    auto idx = deduplicated_schema_without_constants[e.id].first;
                    if (idx != old_idx) {
                        if (old_idx != -1UL)
                            printer.emit_Pop(); // to remove last loaded value
                        printer.emit_Ld_Tup(0, idx);
                        old_idx = idx;
                    }
                }
            }
            printer.emit_Print(ostream_index, e.type);
            if (e.type->is_none() or constant_emitted) {
                printer.emit_Pop(); // to remove NULL pushed by `emit_Print()` or other constant pushed above
                constant_emitted = false;
            }
        }
        if (old_idx != -1UL)
            printer.emit_Pop(); // to remove last loaded value
        for (std::size_t i = 0; i != num_tuples; ++i) {
            printer(args);
            print_op->out << '\n';
        }
    }
}

template<typename Index, typename V8ValueT, bool IsLower>
void m::wasm::detail::index_seek(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    using key_type = Index::key_type;

    /*----- Unpack function parameters -----*/
    auto index_id = info[0].As<v8::BigInt>()->Uint64Value();
    key_type key;
    if constexpr (std::same_as<V8ValueT, v8::BigInt>)
        key = info[1].As<V8ValueT>()->Int64Value();
    else if constexpr (std::same_as<V8ValueT, v8::String>) {
        auto offset = info[1].As<v8::Uint32>()->Value();
        auto &context = WasmEngine::Get_Wasm_Context_By_ID(Module::ID());
        key = reinterpret_cast<const char*>(context.vm.as<uint8_t*>() + offset);
    } else
        key = info[1].As<V8ValueT>()->Value();

    /*----- Obtain index and cast to correct type. -----*/
    auto &context = WasmEngine::Get_Wasm_Context_By_ID(Module::ID());
    auto &index = as<const Index>(context.indexes[index_id]);

    /*----- Seek index and return offset. -----*/
    std::size_t offset = std::distance(
        index.begin(),
        M_CONSTEXPR_COND(IsLower, index.lower_bound(key), index.upper_bound(key))
    );
    M_insist(std::in_range<uint32_t>(offset), "should fit in uint32_t");
    info.GetReturnValue().Set(uint32_t(offset));
}

template<typename Index>
void m::wasm::detail::index_sequential_scan(const v8::FunctionCallbackInfo<v8::Value> &info)
{
    /*----- Unpack function parameters -----*/
    auto index_id = info[0].As<v8::BigInt>()->Uint64Value();
    auto entry_offset = info[1].As<v8::Uint32>()->Value();
    auto address_offset = info[2].As<v8::Uint32>()->Value();
    auto batch_size = info[3].As<v8::Uint32>()->Value();

    /*----- Compute adress to write results to. -----*/
    auto &context = WasmEngine::Get_Wasm_Context_By_ID(Module::ID());
    auto buffer_address = reinterpret_cast<uint32_t*>(context.vm.as<uint8_t*>() + address_offset);

    /*----- Obtain index and cast to correct type. -----*/
    auto &index = as<const Index>(context.indexes[index_id]);

    /*----- Scan index and write result tuple ids to buffer -----*/
    auto it = index.begin() + entry_offset;
    for (uint32_t i = 0; i < batch_size; ++i, ++it)
        buffer_address[i] = it->second;
}


/*======================================================================================================================
 * V8Engine helper classes
 *====================================================================================================================*/

namespace {

struct CollectStringLiterals : ConstOperatorVisitor, ast::ConstASTExprVisitor
{
    private:
    std::unordered_set<const char*> literals_; ///< the collected literals

    public:
    static std::vector<const char*> Collect(const Operator &plan) {
        CollectStringLiterals CSL;
        CSL(plan);
        return { CSL.literals_.begin(), CSL.literals_.end() };
    }

    private:
    CollectStringLiterals() = default;

    using ConstOperatorVisitor::operator();
    using ConstASTExprVisitor::operator();

    void recurse(const Consumer &C) {
        for (auto &c: C.children())
            (*this)(*c);
    }

    /*----- Operator -------------------------------------------------------------------------------------------------*/
    void operator()(const ScanOperator&) override { /* nothing to be done */ }
    void operator()(const CallbackOperator &op) override { recurse(op); }
    void operator()(const PrintOperator &op) override { recurse(op); }
    void operator()(const NoOpOperator &op) override { recurse(op); }
    void operator()(const FilterOperator &op) override {
        (*this)(op.filter());
        recurse(op);
    }
    void operator()(const DisjunctiveFilterOperator &op) override {
        (*this)(op.filter());
        recurse(op);
    }
    void operator()(const JoinOperator &op) override {
        (*this)(op.predicate());
        recurse(op);
    }
    void operator()(const ProjectionOperator &op) override {
        for (auto &p : op.projections())
            (*this)(p.first.get());
        recurse(op);
    }
    void operator()(const LimitOperator &op) override { recurse(op); }
    void operator()(const GroupingOperator &op) override {
        for (auto &[grp, alias] : op.group_by())
            (*this)(grp.get());
        recurse(op);
    }
    void operator()(const AggregationOperator &op) override { recurse(op); }
    void operator()(const SortingOperator &op) override { recurse(op); }

    /*----- CNF ------------------------------------------------------------------------------------------------------*/
    void operator()(const cnf::CNF &cnf) {
        for (auto &clause: cnf) {
            for (auto &pred: clause)
                (*this)(*pred);
        }
    }

    /*----- Expr -----------------------------------------------------------------------------------------------------*/
    void operator()(const ast::ErrorExpr&) override { M_unreachable("no errors at this stage"); }
    void operator()(const ast::Designator&) override { /* nothing to be done */ }
    void operator()(const ast::Constant &e) override {
        if (e.is_string()) {
            auto s = Interpreter::eval(e);
            literals_.emplace(s.as<const char*>());
        }
    }
    void operator()(const ast::FnApplicationExpr&) override { /* nothing to be done */ } // XXX can string literals be arguments?
    void operator()(const ast::UnaryExpr &e) override { (*this)(*e.expr); }
    void operator()(const ast::BinaryExpr &e) override { (*this)(*e.lhs); (*this)(*e.rhs); }
    void operator()(const ast::QueryExpr&) override { /* nothing to be done */ }
};

struct CollectTables : ConstOperatorVisitor
{
    private:
    struct hash
    {
        uint64_t operator()(const std::reference_wrapper<const Table> &r) const {
            StrHash h;
            return h((const char*)(r.get().name()));
        }
    };

    struct equal
    {
        bool operator()(const std::reference_wrapper<const Table> &first,
                        const std::reference_wrapper<const Table> &second) const
        {
            return first.get().name() == second.get().name();
        }
    };


    std::unordered_set<std::reference_wrapper<const Table>, hash, equal> tables_; ///< the collected tables

    public:
    static std::vector<std::reference_wrapper<const Table>> Collect(const Operator &plan) {
        CollectTables CT;
        CT(plan);
        return { CT.tables_.begin(), CT.tables_.end() };
    }

    private:
    CollectTables() = default;

    using ConstOperatorVisitor::operator();

    void recurse(const Consumer &C) {
        for (auto &c: C.children())
            (*this)(*c);
    }

    void operator()(const ScanOperator &op) override { tables_.emplace(op.store().table()); }
    void operator()(const CallbackOperator &op) override { recurse(op); }
    void operator()(const PrintOperator &op) override { recurse(op); }
    void operator()(const NoOpOperator &op) override { recurse(op); }
    void operator()(const FilterOperator &op) override { recurse(op); }
    void operator()(const DisjunctiveFilterOperator &op) override { recurse(op); }
    void operator()(const JoinOperator &op) override { recurse(op); }
    void operator()(const ProjectionOperator &op) override { recurse(op); }
    void operator()(const LimitOperator &op) override { recurse(op); }
    void operator()(const GroupingOperator &op) override { recurse(op); }
    void operator()(const AggregationOperator &op) override { recurse(op); }
    void operator()(const SortingOperator &op) override { recurse(op); }
};


/*======================================================================================================================
 * V8Engine implementation
 *====================================================================================================================*/

V8Engine::V8Engine() { initialize(); }

V8Engine::~V8Engine()
{
    inspector_.reset();
    if (isolate_) {
        M_insist(allocator_);
        isolate_->Dispose();
        delete allocator_;
    }
}

void V8Engine::initialize()
{
    M_insist(not allocator_);
    M_insist(not isolate_);

    /*----- Set V8 flags. --------------------------------------------------------------------------------------------*/
    /* A documentation of these flags can be found at
     * https://chromium.googlesource.com/v8/v8/+/2c22fd50128ad130e9dba77fce828e5661559121/src/flags/flag-definitions.h.*/
    std::ostringstream flags;
    flags << "--stack_size 1000000 ";
    if (options::wasm_adaptive) {
        flags << "--opt "
              << "--liftoff "
              << "--wasm-tier-up "
              << "--wasm-dynamic-tiering "
              << "--wasm-lazy-compilation "; // compile code lazily at runtime if needed
    } else {
        flags << "--no-liftoff "
              << "--no-wasm-lazy-compilation "; // compile code before starting execution
    }
    if (not options::wasm_compilation_cache) {
        flags << "--no-compilation-cache "
              << "--no-wasm-native-module-cache-enabled ";
    }
    if (options::asm_dump) {
        flags << "--code-comments " // include code comments
              << "--print-code ";
    }
    if (options::cdt_port >= 1024) {
        flags << "--wasm-bounds-checks "
              << "--wasm-stack-checks "
              << "--log "
              << "--log-all "
              << "--expose-wasm "
              << "--trace-wasm "
              << "--trace-wasm-instances "
              << "--prof ";
    } else {
        flags << "--no-wasm-bounds-checks "
              << "--no-wasm-stack-checks "
              << "--wasm-simd-ssse3-codegen ";
    }
    v8::V8::SetFlagsFromString(flags.str().c_str());

    v8::Isolate::CreateParams create_params;
    create_params.array_buffer_allocator = allocator_ = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
    isolate_ = v8::Isolate::New(create_params);
}

void V8Engine::compile(const m::MatchBase &plan) const
{
#if 1
    /*----- Add print function. --------------------------------------------------------------------------------------*/
    Module::Get().emit_function_import<void(uint32_t)>("print");
    Module::Get().emit_function_import<void(uint32_t, uint32_t)>("print_memory_consumption");
#endif

    /*----- Emit code for run function which computes the last pipeline and calls other pipeline functions. ----------*/
    FUNCTION(run, void(void))
    {
        auto S = CodeGenContext::Get().scoped_environment(); // create scoped environment for this function
        plan.execute(setup_t::Make_Without_Parent(), pipeline_t(), teardown_t::Make_Without_Parent()); // emit code
    }

    /*----- Create function `main` which executes the given query. ---------------------------------------------------*/
    m::wasm::Function<uint32_t(uint32_t)> main("main");
    BLOCK_OPEN(main.body())
    {
        auto S = CodeGenContext::Get().scoped_environment(); // create scoped environment for this function
        run(); // call run function
        if (Options::Get().statistics) {
            std::cout << "Pre-allocated memory overall consumption: "
                      << Module::Allocator().pre_allocated_memory_consumption() / (1024.0 * 1024.0)
                      << " MiB" << std::endl;
            Module::Get().emit_call<void>("print_memory_consumption",
                                          Module::Allocator().allocated_memory_consumption(),
                                          Module::Allocator().allocated_memory_peak());
        }
        main.emit_return(CodeGenContext::Get().num_tuples()); // return size of result set
    }

    /*----- Export main. ---------------------------------------------------------------------------------------------*/
    Module::Get().emit_function_export("main");

    /*----- Perform memory pre-allocations. --------------------------------------------------------------------------*/
    Module::Allocator().perform_pre_allocations();

    /*----- Dump the generated WebAssembly code ----------------------------------------------------------------------*/
    if (options::wasm_dump)
        Module::Get().dump_all(std::cout);

#ifndef NDEBUG
    /*----- Validate module before optimization. ---------------------------------------------------------------------*/
    if (not Module::Validate()) {
        Module::Get().dump_all();
        throw std::logic_error("invalid module");
    }
#endif

    /*----- Optimize module. -----------------------------------------------------------------------------------------*/
#ifndef NDEBUG
    std::ostringstream dump_before_opt;
    Module::Get().dump(dump_before_opt);
#endif
    if (options::wasm_optimization_level)
        Module::Optimize(options::wasm_optimization_level);

#ifndef NDEBUG
    /*----- Validate module after optimization. ----------------------------------------------------------------------*/
    if (options::wasm_optimization_level and not Module::Validate()) {
        std::cerr << "Module invalid after optimization!" << std::endl;
        std::cerr << "WebAssembly before optimization:\n" << dump_before_opt.str() << std::endl;
        std::cerr << "WebAssembly after optimization:\n";
        Module::Get().dump(std::cerr);
        throw std::logic_error("invalid module");
    }
#endif
}

void V8Engine::execute(const m::MatchBase &plan)
{
    Catalog &C = Catalog::Get();

    Module::Init();
    CodeGenContext::Init(); // fresh context

    M_insist(bool(isolate_), "must have an isolate");
    v8::Locker locker(isolate_);
    isolate_->Enter();

    {
        /* Create required V8 scopes. */
        v8::Isolate::Scope isolate_scope(isolate_);
        v8::HandleScope handle_scope(isolate_); // tracks and disposes of all object handles

        /* Create global template and context. */
        v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate_);
        global->Set(isolate_, "set_wasm_instance_raw_memory", v8::FunctionTemplate::New(isolate_, set_wasm_instance_raw_memory));
        global->Set(isolate_, "read_result_set", v8::FunctionTemplate::New(isolate_, read_result_set));

#define CREATE_TEMPLATES(IDXTYPE, KEYTYPE, V8TYPE, IDXNAME, SUFFIX) \
        global->Set(isolate_, M_STR(idx_lower_bound_##IDXNAME##_##SUFFIX), v8::FunctionTemplate::New(isolate_, index_seek<IDXTYPE<KEYTYPE>, V8TYPE, true>)); \
        global->Set(isolate_, M_STR(idx_upper_bound_##IDXNAME##_##SUFFIX), v8::FunctionTemplate::New(isolate_, index_seek<IDXTYPE<KEYTYPE>, V8TYPE, false>)); \
        global->Set(isolate_, M_STR(idx_scan_##IDXNAME##_##SUFFIX),        v8::FunctionTemplate::New(isolate_, index_sequential_scan<IDXTYPE<KEYTYPE>>))

        CREATE_TEMPLATES(idx::ArrayIndex, bool,        v8::Boolean, array, b);
        CREATE_TEMPLATES(idx::ArrayIndex, int8_t,      v8::Int32,   array, i1);
        CREATE_TEMPLATES(idx::ArrayIndex, int16_t,     v8::Int32,   array, i2);
        CREATE_TEMPLATES(idx::ArrayIndex, int32_t,     v8::Int32,   array, i4);
        CREATE_TEMPLATES(idx::ArrayIndex, int64_t,     v8::BigInt,  array, i8);
        CREATE_TEMPLATES(idx::ArrayIndex, float,       v8::Number,  array, f);
        CREATE_TEMPLATES(idx::ArrayIndex, double,      v8::Number,  array, d);
        CREATE_TEMPLATES(idx::ArrayIndex, const char*, v8::String,  array, p);
        CREATE_TEMPLATES(idx::RecursiveModelIndex, int8_t,      v8::Int32,  rmi, i1);
        CREATE_TEMPLATES(idx::RecursiveModelIndex, int16_t,     v8::Int32,  rmi, i2);
        CREATE_TEMPLATES(idx::RecursiveModelIndex, int32_t,     v8::Int32,  rmi, i4);
        CREATE_TEMPLATES(idx::RecursiveModelIndex, int64_t,     v8::BigInt, rmi, i8);
        CREATE_TEMPLATES(idx::RecursiveModelIndex, float,       v8::Number, rmi, f);
        CREATE_TEMPLATES(idx::RecursiveModelIndex, double,      v8::Number, rmi, d);
#undef CREATE_TEMPLATES

        v8::Local<v8::Context> context = v8::Context::New(isolate_, /* extensions= */ nullptr, global);
        v8::Context::Scope context_scope(context);

        /* Create the import object for instantiating the WebAssembly module. */
        WasmContext::config_t wasm_config{0};
        if (options::cdt_port < 1024)
            wasm_config |= WasmContext::TRAP_GUARD_PAGES;
        auto &wasm_context = Create_Wasm_Context_For_ID(Module::ID(), plan, wasm_config);

        auto imports = v8::Object::New(isolate_);
        auto env = create_env(*isolate_, plan);
        M_DISCARD imports->Set(context, mkstr(*isolate_, "imports"), env);

        /* Map the remaining address space to the output buffer. */
        M_insist(Is_Page_Aligned(wasm_context.heap));
        const auto bytes_remaining = wasm_context.vm.size() - wasm_context.heap;
        memory::Memory mem = Catalog::Get().allocator().allocate(bytes_remaining);
        mem.map(bytes_remaining, 0, wasm_context.vm, wasm_context.heap);

        auto compile_time = C.timer().create_timing("Compile SQL to machine code");
        /* Compile the plan and thereby build the Wasm module. */
        M_TIME_EXPR(compile(plan), "|- Compile SQL to WebAssembly", C.timer());
        /* Create a WebAssembly instance object. */
        auto instance = M_TIME_EXPR(instantiate(*isolate_, imports), " ` Compile WebAssembly to machine code", C.timer());
        compile_time.stop();

        /* Set the underlying memory for the instance. */
        v8::SetWasmInstanceRawMemory(instance, wasm_context.vm.as<uint8_t*>(), wasm_context.vm.size());

        /* Get the exports of the created WebAssembly instance. */
        auto exports = instance->Get(context, mkstr(*isolate_, "exports")).ToLocalChecked().As<v8::Object>();
        auto main = exports->Get(context, mkstr(*isolate_, "main")).ToLocalChecked().As<v8::Function>();

        /* If a debugging port is specified, set up the inspector and start it. */
        if (options::cdt_port >= 1024 and not inspector_)
            inspector_ = std::make_unique<V8InspectorClientImpl>(options::cdt_port, isolate_);
        if (bool(inspector_)) {
            run_inspector(*inspector_, *isolate_, env);
            return;
        }

        /* Invoke the exported function `main` of the module. */
        args_t args { v8::Int32::New(isolate_, wasm_context.id), };
        const uint32_t num_rows =
            M_TIME_EXPR(main->Call(context, context->Global(), 1, args).ToLocalChecked().As<v8::Uint32>()->Value(),
                        "Execute machine code", C.timer());

        /* Print total number of result tuples. */
        auto &root_op = plan.get_matched_root();
        if (auto print_op = cast<const PrintOperator>(&root_op)) {
            if (not Options::Get().quiet)
                print_op->out << num_rows << " rows\n";
        } else if (auto noop_op = cast<const NoOpOperator>(&root_op)) {
            if (not Options::Get().quiet)
                noop_op->out << num_rows << " rows\n";
        }
        Dispose_Wasm_Context(wasm_context);
    }

    isolate_->Exit();
    CodeGenContext::Dispose();
    Module::Dispose();
}

__attribute__((constructor(101)))
static void create_V8Engine()
{
    V8Engine::PLATFORM_ = v8::platform::NewDefaultPlatform().release();
    v8::V8::InitializePlatform(V8Engine::PLATFORM_);
    v8::V8::SetFlagsFromString("--no-freeze-flags-after-init"); // allow changing flags after initialization
    v8::V8::Initialize();
}

__attribute__((destructor(101)))
static void destroy_V8Engine()
{
    v8::V8::Dispose();
    v8::V8::DisposePlatform();
}

__attribute__((constructor(202)))
static void register_WasmV8()
{
    Catalog &C = Catalog::Get();
    C.register_wasm_backend<V8Engine>(C.pool("WasmV8"), "WebAssembly backend using Google's V8 engine");

    /*----- Command-line arguments -----------------------------------------------------------------------------------*/
    C.arg_parser().add<int>(
        /* group=       */ "Wasm",
        /* short=       */ nullptr,
        /* long=        */ "--wasm-opt",
        /* description= */ "set the optimization level for Wasm modules (0, 1, or 2)",
                           [] (int i) { options::wasm_optimization_level = i; }
    );
    C.arg_parser().add<bool>(
        /* group=       */ "WasmV8",
        /* short=       */ nullptr,
        /* long=        */ "--wasm-adaptive",
        /* description= */ "enable adaptive execution of Wasm with Liftoff and dynamic tier-up",
                           [] (bool b) { options::wasm_adaptive = b; }
    );
    C.arg_parser().add<bool>(
        /* group=       */ "WasmV8",
        /* short=       */ nullptr,
        /* long=        */ "--no-wasm-compilation-cache",
        /* description= */ "disable V8's compilation cache",
                           [] (bool) { options::wasm_compilation_cache = false; }
    );
    C.arg_parser().add<bool>(
        /* group=       */ "Wasm",
        /* short=       */ nullptr,
        /* long=        */ "--wasm-dump",
        /* description= */ "dump the generated WebAssembly code to stdout",
                           [] (bool b) { options::wasm_dump = b; }
    );
    C.arg_parser().add<bool>(
        /* group=       */ "WasmV8",
        /* short=       */ nullptr,
        /* long=        */ "--asm-dump",
        /* description= */ "dump the generated assembly code to stdout",
                           [] (bool b) { options::asm_dump = b; }
    );
    C.arg_parser().add<int>(
        /* group=       */ "WasmV8",
        /* short=       */ nullptr,
        /* long=        */ "--CDT",
        /* description= */ "specify the port for debugging via ChromeDevTools",
                           [] (int i) { options::cdt_port = i; }
    );
}

}


/*======================================================================================================================
 * Implementation of helper methods
 *====================================================================================================================*/

v8::Local<v8::String> m::wasm::detail::mkstr(v8::Isolate &isolate, const std::string &str)
{
    return to_v8_string(&isolate, str);
}

v8::Local<v8::WasmModuleObject> m::wasm::detail::instantiate(v8::Isolate &isolate, v8::Local<v8::Object> imports)
{
    auto Ctx = isolate.GetCurrentContext();
    auto [binary_addr, binary_size] = Module::Get().binary();
    auto bs = v8::ArrayBuffer::NewBackingStore(
        /* data =        */ binary_addr,
        /* byte_length=  */ binary_size,
        /* deleter=      */ v8::BackingStore::EmptyDeleter,
        /* deleter_data= */ nullptr
    );
    auto buffer = v8::ArrayBuffer::New(&isolate, std::move(bs));

    if (Options::Get().statistics)
        std::cout << "Wasm code size: " << binary_size << std::endl;

    args_t module_args { buffer };
    auto wasm = Ctx->Global()->Get(Ctx, mkstr(isolate, "WebAssembly")).ToLocalChecked().As<v8::Object>(); // WebAssembly class
    auto wasm_module = wasm->Get(Ctx, mkstr(isolate, "Module")).ToLocalChecked().As<v8::Object>()
                           ->CallAsConstructor(Ctx, 1, module_args).ToLocalChecked().As<v8::WasmModuleObject>();
    free(binary_addr);

    if (Options::Get().statistics)
        std::cout << "Machine code size: " << wasm_module->GetCompiledModule().Serialize().size << std::endl;

    args_t instance_args { wasm_module, imports };
    return wasm->Get(Ctx, mkstr(isolate, "Instance")).ToLocalChecked().As<v8::Object>()
               ->CallAsConstructor(Ctx, 2, instance_args).ToLocalChecked().As<v8::WasmModuleObject>();
}

v8::Local<v8::Object> m::wasm::detail::create_env(v8::Isolate &isolate, const m::MatchBase &plan)
{
    auto &context = WasmEngine::Get_Wasm_Context_By_ID(Module::ID());
    auto Ctx = isolate.GetCurrentContext();
    auto env = v8::Object::New(&isolate);

    /* Map accessed tables into the Wasm module. */
    auto tables = CollectTables::Collect(plan.get_matched_root());
    for (auto &table : tables) {
        auto off = context.map_table(table.get());

        /* Add memory address to env. */
        std::ostringstream oss;
        oss << table.get().name() << "_mem";
        M_DISCARD env->Set(Ctx, to_v8_string(&isolate, oss.str()), v8::Int32::New(&isolate, off));
        Module::Get().emit_import<void*>(oss.str().c_str());

        /* Add table size (num_rows) to env. */
        oss.str("");
        oss << table.get().name() << "_num_rows";
        M_DISCARD env->Set(Ctx, to_v8_string(&isolate, oss.str()), v8::Int32::New(&isolate, table.get().store().num_rows()));
        Module::Get().emit_import<uint32_t>(oss.str().c_str());
    }

    /* Map all string literals into the Wasm module. */
    M_insist(Is_Page_Aligned(context.heap));
    auto literals = CollectStringLiterals::Collect(plan.get_matched_root());
    std::size_t bytes = 0;
    for (auto literal : literals)
        bytes += strlen(literal) + 1;
    auto aligned_bytes = Ceil_To_Next_Page(bytes);
    if (aligned_bytes) {
        auto base_addr = context.vm.as<uint8_t*>() + context.heap;
        M_DISCARD mmap(base_addr, aligned_bytes, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_ANON|MAP_PRIVATE, -1, 0);
        char *start_addr = reinterpret_cast<char*>(base_addr);
        char *dst = start_addr;
        for (auto literal : literals) {
            CodeGenContext::Get().add_literal(literal, context.heap + (dst - start_addr)); // add literal
            dst = stpcpy(dst, literal) + 1; // copy into sequential memory
        }
        context.heap += aligned_bytes;
        context.install_guard_page();
    }
    M_insist(Is_Page_Aligned(context.heap));

    /* Add functions to environment. */
    Module::Get().emit_function_import<void(void*,uint32_t)>("read_result_set");

#define EMIT_FUNC_IMPORTS(KEYTYPE, IDXNAME, SUFFIX) \
    Module::Get().emit_function_import<uint32_t(std::size_t,KEYTYPE)>(M_STR(idx_lower_bound_##IDXNAME##_##SUFFIX)); \
    Module::Get().emit_function_import<uint32_t(std::size_t,KEYTYPE)>(M_STR(idx_upper_bound_##IDXNAME##_##SUFFIX)); \
    Module::Get().emit_function_import<void(std::size_t,uint32_t,void*,uint32_t)>(M_STR(idx_scan_##IDXNAME##_##SUFFIX))

    EMIT_FUNC_IMPORTS(bool,        array, b);
    EMIT_FUNC_IMPORTS(int8_t,      array, i1);
    EMIT_FUNC_IMPORTS(int16_t,     array, i2);
    EMIT_FUNC_IMPORTS(int32_t,     array, i4);
    EMIT_FUNC_IMPORTS(int64_t,     array, i8);
    EMIT_FUNC_IMPORTS(float,       array, f);
    EMIT_FUNC_IMPORTS(double,      array, d);
    EMIT_FUNC_IMPORTS(const char*, array, p);
    EMIT_FUNC_IMPORTS(int8_t,      rmi, i1);
    EMIT_FUNC_IMPORTS(int16_t,     rmi, i2);
    EMIT_FUNC_IMPORTS(int32_t,     rmi, i4);
    EMIT_FUNC_IMPORTS(int64_t,     rmi, i8);
    EMIT_FUNC_IMPORTS(float,       rmi, f);
    EMIT_FUNC_IMPORTS(double,      rmi, d);
#undef EMIT_FUNC_IMPORTS

#define ADD_FUNC(FUNC, NAME) { \
    auto func = v8::Function::New(Ctx, (FUNC)).ToLocalChecked(); \
    env->Set(Ctx, mkstr(isolate, NAME), func).Check(); \
}
#define ADD_FUNC_(FUNC) ADD_FUNC(FUNC, #FUNC)
    ADD_FUNC_(insist)
    ADD_FUNC_(print)
    ADD_FUNC_(print_memory_consumption)
    ADD_FUNC_(read_result_set)
    ADD_FUNC(_throw, "throw")

#define ADD_FUNCS(IDXTYPE, KEYTYPE, V8TYPE, IDXNAME, SUFFIX) \
    ADD_FUNC(index_seek<M_COMMA(IDXTYPE<KEYTYPE>) M_COMMA(V8TYPE) true>,  M_STR(idx_lower_bound_##IDXNAME##_##SUFFIX)) \
    ADD_FUNC(index_seek<M_COMMA(IDXTYPE<KEYTYPE>) M_COMMA(V8TYPE) false>, M_STR(idx_upper_bound_##IDXNAME##_##SUFFIX)) \
    ADD_FUNC(index_sequential_scan<IDXTYPE<KEYTYPE>>, M_STR(idx_scan_##IDXNAME##_##SUFFIX))

    ADD_FUNCS(idx::ArrayIndex,          bool,        v8::Boolean, array, b);
    ADD_FUNCS(idx::ArrayIndex,          int8_t,      v8::Int32,   array, i1);
    ADD_FUNCS(idx::ArrayIndex,          int16_t,     v8::Int32,   array, i2);
    ADD_FUNCS(idx::ArrayIndex,          int32_t,     v8::Int32,   array, i4);
    ADD_FUNCS(idx::ArrayIndex,          int64_t,     v8::BigInt,  array, i8);
    ADD_FUNCS(idx::ArrayIndex,          float,       v8::Number,  array, f);
    ADD_FUNCS(idx::ArrayIndex,          double,      v8::Number,  array, d);
    ADD_FUNCS(idx::ArrayIndex,          const char*, v8::String,  array, p);
    ADD_FUNCS(idx::RecursiveModelIndex, int8_t,      v8::Int32,   rmi, i1);
    ADD_FUNCS(idx::RecursiveModelIndex, int16_t,     v8::Int32,   rmi, i2);
    ADD_FUNCS(idx::RecursiveModelIndex, int32_t,     v8::Int32,   rmi, i4);
    ADD_FUNCS(idx::RecursiveModelIndex, int64_t,     v8::BigInt,  rmi, i8);
    ADD_FUNCS(idx::RecursiveModelIndex, float,       v8::Number,  rmi, f);
    ADD_FUNCS(idx::RecursiveModelIndex, double,      v8::Number,  rmi, d);
#undef ADD_FUNCS
#undef ADD_FUNC_
#undef ADD_FUNC

    return env;
}

v8::Local<v8::String> m::wasm::detail::to_json(v8::Isolate &isolate, v8::Local<v8::Value> val)
{
    M_insist(&isolate);
    auto Ctx = isolate.GetCurrentContext();
    return v8::JSON::Stringify(Ctx, val).ToLocalChecked();
}

std::string m::wasm::detail::create_js_debug_script(v8::Isolate &isolate, v8::Local<v8::Object> env,
                                                    const WasmEngine::WasmContext &wasm_context)
{
    std::ostringstream oss;

    auto [binary_addr, binary_size] = Module::Get().binary();

    auto json = to_json(isolate, env);
    std::string env_str = *v8::String::Utf8Value(&isolate, json);
    if (env_str != "{}") env_str.insert(env_str.length() - 1, ",");
    env_str.insert(env_str.length() - 1, "\"insist\": function (arg) { assert(arg); },");
    env_str.insert(env_str.length() - 1, "\"print\": function (arg) { console.log(arg); },");
    env_str.insert(env_str.length() - 1, "\"throw\": function (ex) { console.error(ex); },");
    env_str.insert(env_str.length() - 1, "\"read_result_set\": read_result_set,");

    /* Construct import object. */
    oss << "\
let importObject = { \"imports\": " << env_str << " };\n\
const bytes = Uint8Array.from([";
    for (auto p = binary_addr, end = p + binary_size; p != end; ++p) {
        if (p != binary_addr) oss << ", ";
        oss << unsigned(*p);
    }
    free(binary_addr);
    /* Emit code to instantiate module and invoke exported `run()` function. */
    oss << "]);\n\
WebAssembly.compile(bytes).then(\n\
    (module) => WebAssembly.instantiate(module, importObject),\n\
    (error) => console.error(`An error occurred during module compilation: ${error}`)\n\
).then(\n\
    function(instance) {\n\
        set_wasm_instance_raw_memory(instance, " << wasm_context.id << ");\n\
        const num_tuples = instance.exports.main();\n\
        console.log('The result set contains %i tuples.', num_tuples);\n\
        debugger;\n\
    },\n\
    (error) => console.error(`An error occurred during module instantiation: ${error}`)\n\
);\n\
debugger;";

    /* Create a new temporary file. */
    const char *name = "query.js";
    std::ofstream file(name, std::ios_base::out);
    if (not file)
        throw runtime_error("I/O error");
    std::cerr << "Creating debug JS script " << name << std::endl;

    /* Write the JS code to instantiate the module and invoke `run()` to the temporary file. */
    file << oss.str();
    if (not file)
        throw runtime_error("I/O error");
    file.flush();
    if (not file)
        throw runtime_error("I/O error");

    /* Return the name of the temporary file. */
    return std::string(name);
}

void m::wasm::detail::run_inspector(V8InspectorClientImpl &inspector, v8::Isolate &isolate, v8::Local<v8::Object> env)
{
    auto Ctx = isolate.GetCurrentContext();
    auto &wasm_context = WasmEngine::Get_Wasm_Context_By_ID(Module::ID());

    inspector.register_context(Ctx);
    inspector.start([&]() {
        /* Create JS script file that instantiates the Wasm module and invokes `main()`. */
        auto filename = create_js_debug_script(isolate, env, wasm_context);
        /* Create a `v8::Script` for that JS file. */
        std::ifstream js_in(filename);
        std::string js(std::istreambuf_iterator<char>(js_in), std::istreambuf_iterator<char>{});
        v8::Local<v8::String> js_src = mkstr(isolate, js);
        std::string path = std::string("file://./") + filename;
        v8::ScriptOrigin js_origin = v8::ScriptOrigin(&isolate, mkstr(isolate, path));
        auto script = v8::Script::Compile(Ctx, js_src, &js_origin);
        if (script.IsEmpty())
            throw std::runtime_error("failed to compile script");
        /* Execute the `v8::Script`. */
        auto result = script.ToLocalChecked()->Run(Ctx);
        if (result.IsEmpty())
            throw std::runtime_error("execution failed");
    });
    inspector.deregister_context(Ctx);
}
