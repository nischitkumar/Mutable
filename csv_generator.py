import csv
import random
import datetime

def generate_large_csv(filename, num_rows):
    with open(filename, 'w', newline='') as csvfile:
        writer = csv.writer(csvfile)

        # Write header row
        if filename == "customers.csv":
            writer.writerow(["customer_id", "name", "city", "age", "account_balance"])
            for i in range(1, num_rows + 1):
                name = f"Customer {i}"
                city = random.choice(["New York", "Los Angeles", "Chicago", "Houston"])
                age = random.randint(18, 70)
                balance = round(random.uniform(0, 10000), 2)
                writer.writerow([i, name, city, age, balance])

        elif filename == "orders.csv":
             writer.writerow(["order_id", "customer_id", "order_date", "amount", "shipping_address"])
             for i in range(1, num_rows + 1):
                customer_id = random.randint(1, 1000)  # Up to 1000 customers (adjust as needed)
                order_date = datetime.date(2023, 1, 1) + datetime.timedelta(days=random.randint(0, 365))
                amount = round(random.uniform(10, 500), 2)
                shipping_address = f"{random.randint(100, 999)} Random St"
                writer.writerow([i, customer_id, order_date, amount, shipping_address])

        elif filename == "products.csv":
            writer.writerow(["product_id", "product_name", "category", "price", "inventory"])
            categories = ["Electronics", "Clothing", "Home Goods", "Books", "Sports"]
            for i in range(1, num_rows + 1):
                product_name = f"Product {i}"
                category = random.choice(categories)
                price = round(random.uniform(5, 200), 2)
                inventory = random.randint(10, 500)
                writer.writerow([f"P{i}", product_name, category, price, inventory])
        elif filename == "categories.csv": #no longer needing this cuz the new csv does not learn database name
            writer.writerow(["category_id", "category_name", "description"])
            categories = {
                1: ("Electronics", "Electronic devices and accessories"),
                2: ("Clothing", "Apparel and fashion items"),
                3: ("Home Goods", "Household items and furniture"),
                4: ("Books", "Literature and educational materials"),
                5: ("Sports", "Sporting equipment and apparel")
            }
            for category_id, (category_name, description) in categories.items():
                writer.writerow([category_id, category_name, description])


# Generate the CSV files
num_rows = 90000
generate_large_csv("customers.csv", num_rows)
generate_large_csv("orders.csv", num_rows)
generate_large_csv("products.csv", num_rows)
generate_large_csv("categories.csv",5)

print("CSV files generated.")
