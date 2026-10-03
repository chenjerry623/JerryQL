-- Secondary indexes: creation, planning, maintenance, errors.
CREATE TABLE orders (id INT PRIMARY KEY, city TEXT, amount INT);
INSERT INTO orders VALUES (1, 'Tokyo', 100), (2, 'Lima', 250), (3, 'Tokyo', 75), (4, 'Paris', 300),
  (5, 'Lima', 25), (6, 'Tokyo', 300), (7, 'Oslo', 150);
CREATE INDEX orders_city ON orders (city);
CREATE INDEX orders_amount ON orders (amount);
EXPLAIN SELECT * FROM orders WHERE city = 'Tokyo';
SELECT * FROM orders WHERE city = 'Tokyo';
EXPLAIN SELECT id, amount FROM orders WHERE amount >= 100 AND amount < 300;
SELECT id, amount FROM orders WHERE amount >= 100 AND amount < 300;
SELECT id FROM orders WHERE city > 'M';
EXPLAIN SELECT id FROM orders WHERE id = 3 AND city = 'Tokyo';
EXPLAIN SELECT id FROM orders WHERE amount > 50 AND city = 'Lima';
SELECT city, COUNT(*), SUM(amount) FROM orders WHERE city >= 'L' AND city < 'P' GROUP BY city;
-- Writes keep indexes current.
UPDATE orders SET city = 'Kyoto' WHERE id = 1;
UPDATE orders SET id = id + 100 WHERE city = 'Lima';
DELETE FROM orders WHERE amount = 300;
SELECT id, city FROM orders WHERE city = 'Tokyo';
SELECT id, city FROM orders WHERE city = 'Kyoto';
SELECT id, city FROM orders WHERE city = 'Lima';
SELECT id FROM orders WHERE amount > 1000;
DROP INDEX orders_city;
EXPLAIN SELECT id FROM orders WHERE city = 'Lima';
-- Errors
CREATE INDEX orders_amount ON orders (city);
CREATE INDEX again ON orders (amount);
CREATE INDEX by_id ON orders (id);
CREATE INDEX nope ON orders (missing);
DROP INDEX orders_city;
