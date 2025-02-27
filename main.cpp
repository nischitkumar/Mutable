#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <map>
#include <memory>
#include <stdexcept>
#include <cstdlib>
#include <algorithm>

// ---------- CSV Reading Utility ----------

/// To read a CSV file
std::vector<std::vector<std::string>> readCSV(const std::string &filename) {
    std::vector<std::vector<std::string>> data;
    std::ifstream file(filename);
    if (!file)
        throw std::runtime_error("Cannot open file: " + filename);
    std::string line;
    while (std::getline(file, line)) {
        std::vector<std::string> row;
        std::istringstream linestream(line);
        std::string token;
        while (std::getline(linestream, token, ',')) {
            // trim spaces (if needed)
            token.erase(token.begin(), std::find_if(token.begin(), token.end(), [](unsigned char ch){ return !std::isspace(ch); }));
            token.erase(std::find_if(token.rbegin(), token.rend(), [](unsigned char ch){ return !std::isspace(ch); }).base(), token.end());
            row.push_back(token);
        }
        if (!row.empty())
            data.push_back(row);
    }
    return data;
}

// ---------- SPN Node Classes ----------

/// Abstract base class for SPN nodes.
class SPNNode {
public:
    virtual double evaluate(const std::vector<std::string> &tuple) const = 0;
    /// For incremental update; delta is +1 for insertion, -1 for deletion.
    virtual void update(const std::vector<std::string> &tuple, int delta) = 0;
    virtual ~SPNNode() {}
};

/// Leaf node: models a single column with a frequency table.
class LeafNode : public SPNNode {
private:
    int colIndex;
    std::map<std::string, int> frequency;
    int total;
public:
    LeafNode(int col) : colIndex(col), total(0) {}

    // Train the leaf using all data rows (i.e. compute frequencies)
    void train(const std::vector<std::vector<std::string>> &data) {
        for (const auto &row : data) {
            if (colIndex < row.size()) {
                frequency[row[colIndex]]++;
                total++;
            }
        }
    }

    // Evaluate returns the empirical probability of the value in the tuple.
    double evaluate(const std::vector<std::string> &tuple) const override {
        if (colIndex >= tuple.size() || total == 0)
            return 0.0;
        auto it = frequency.find(tuple[colIndex]);
        if (it != frequency.end())
            return static_cast<double>(it->second) / total;
        else
            return 0.0;
    }

    // Update the frequency counts based on the new (or deleted) tuple.
    void update(const std::vector<std::string> &tuple, int delta) override {
        if (colIndex >= tuple.size())
            return;
        frequency[tuple[colIndex]] += delta;
        total += delta;
        if (frequency[tuple[colIndex]] < 0)
            frequency[tuple[colIndex]] = 0;
        if (total < 0)
            total = 0;
    }
};

/// Product node: assumes its children are independent.
class ProductNode : public SPNNode {
private:
    std::vector<std::shared_ptr<SPNNode>> children;
public:
    ProductNode(const std::vector<std::shared_ptr<SPNNode>> &children_) : children(children_) {}

    double evaluate(const std::vector<std::string> &tuple) const override {
        double prod = 1.0;
        for (const auto &child : children)
            prod *= child->evaluate(tuple);
        return prod;
    }

    void update(const std::vector<std::string> &tuple, int delta) override {
        // For product nodes, propagate the update to all children.
        for (auto &child : children)
            child->update(tuple, delta);
    }
};

/// Sum node: represents a mixture (e.g. clusters). For simplicity, we update all children.
class SumNode : public SPNNode {
private:
    std::vector<std::shared_ptr<SPNNode>> children;
    std::vector<double> weights; // assumed normalized to sum to 1.
public:
    SumNode(const std::vector<std::shared_ptr<SPNNode>> &children_, const std::vector<double> &weights_)
        : children(children_), weights(weights_) {
        if (children.size() != weights.size())
            throw std::invalid_argument("Children and weights size mismatch.");
        double sum = 0;
        for (double w : weights)
            sum += w;
        for (auto &w : this->weights)
            w /= sum;
    }

    double evaluate(const std::vector<std::string> &tuple) const override {
        double sumEval = 0.0;
        for (size_t i = 0; i < children.size(); i++)
            sumEval += weights[i] * children[i]->evaluate(tuple);
        return sumEval;
    }

    void update(const std::vector<std::string> &tuple, int delta) override {
        // For a sum node, a proper implementation would assign the update to the “nearest” child (e.g., based on distance).
        // Here, for simplicity, we update all children.
        for (auto &child : children)
            child->update(tuple, delta);
    }
};

// ---------- SPN Model and Learning ----------

/// A simple SPN model that consists of independent leaf nodes combined in a product node.
/// (This corresponds to a basic “DeepDB” where we ignore inter-column correlations.)
struct SPNModel {
    std::shared_ptr<SPNNode> root;
    // Keep direct access to leaf nodes for individual column queries.
    std::vector<std::shared_ptr<LeafNode>> leaves;
};

/// Build a simple SPN from CSV data:
/// For each column, a LeafNode is trained (i.e. frequency count is computed)
/// and then they are combined into a ProductNode.
SPNModel buildSPN(const std::vector<std::vector<std::string>> &data) {
    if (data.empty())
        throw std::runtime_error("No data provided.");
    int numColumns = data[0].size();
    std::vector<std::shared_ptr<LeafNode>> leaves;
    for (int col = 0; col < numColumns; col++) {
        auto leaf = std::make_shared<LeafNode>(col);
        leaf->train(data);
        leaves.push_back(leaf);
    }
    // Combine all leaves in a product node (assumes independence across columns).
    std::vector<std::shared_ptr<SPNNode>> children(leaves.begin(), leaves.end());
    auto prod = std::make_shared<ProductNode>(children);
    return SPNModel{prod, leaves};
}

// ---------- Query and Update Functions ----------

/// Evaluate a simple equality predicate on a given column.
/// Returns the probability estimated by the leaf node for that value.
double queryColumnProbability(const SPNModel &model, int col, const std::string &value) {
    if (col < 0 || col >= static_cast<int>(model.leaves.size()))
        throw std::out_of_range("Invalid column index for query.");
    // Create a dummy tuple with the given value at the specified column.
    // (Other columns are ignored in this simple example.)
    std::vector<std::string> dummyTuple(model.leaves.size(), "");
    dummyTuple[col] = value;
    return model.leaves[col]->evaluate(dummyTuple);
}

/// Incrementally update the model with a new tuple (insert) or deletion.
/// Here, delta = +1 for insertion and -1 for deletion.
void updateModel(SPNModel &model, const std::vector<std::string> &tuple, int delta) {
    model.root->update(tuple, delta);
}

// ---------- Main: CSV-based SPN Learning and Querying ----------

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <csv_file>\n";
        return EXIT_FAILURE;
    }

    std::string filename = argv[1];
    std::vector<std::vector<std::string>> data;

    try {
        data = readCSV(filename);
    } catch (const std::exception &ex) {
        std::cerr << "Error reading CSV: " << ex.what() << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "Read " << data.size() << " rows from " << filename << ".\n";

    // Build the (very simple) SPN model from CSV data.
    SPNModel model = buildSPN(data);
    std::cout << "SPN model built (using independent column leafs).\n";

    // Command-line interface for querying and updating.
    while (true) {
        std::cout << "\nSelect an option:\n"
                  << "1. Query probability for a column equality predicate\n"
                  << "2. Insert a new tuple (update model)\n"
                  << "3. Exit\n"
                  << "Choice: ";
        int choice;
        std::cin >> choice;
        if (choice == 1) {
            int col;
            std::string val;
            std::cout << "Enter column index (0-based): ";
            std::cin >> col;
            std::cout << "Enter value to query: ";
            std::cin >> val;
            try {
                double prob = queryColumnProbability(model, col, val);
                std::cout << "Estimated probability: " << prob << "\n";
            } catch (const std::exception &ex) {
                std::cout << "Error during query: " << ex.what() << "\n";
            }
        } else if (choice == 2) {
            std::cout << "Enter new tuple values separated by spaces (" << model.leaves.size() << " values expected):\n";
            std::vector<std::string> newTuple;
            for (int i = 0; i < static_cast<int>(model.leaves.size()); i++) {
                std::string token;
                std::cin >> token;
                newTuple.push_back(token);
            }
            // Update the model (insertion: delta = +1)
            updateModel(model, newTuple, +1);
            std::cout << "Model updated with new tuple.\n";
        } else if (choice == 3) {
            break;
        } else {
            std::cout << "Invalid option.\n";
        }
    }

    std::cout << "Exiting.\n";
    return EXIT_SUCCESS;
}
