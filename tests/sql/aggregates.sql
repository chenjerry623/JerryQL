-- Aggregates, GROUP BY, HAVING, ORDER BY aliases.
CREATE TABLE sales (id INT PRIMARY KEY, city TEXT, rep TEXT, amount INT);
INSERT INTO sales VALUES (1, 'Tokyo', 'Ada', 100), (2, 'Tokyo', 'Ken', 250), (3, 'Paris', 'Ada', 75),
  (4, 'Lima', 'Ken', 300), (5, 'Paris', 'Ada', 25), (6, 'Lima', 'Grace', 300);
SELECT COUNT(*), COUNT(amount), SUM(amount), MIN(amount), MAX(amount), AVG(amount) FROM sales;
SELECT MIN(city), MAX(rep) FROM sales;
SELECT city, COUNT(*) AS n, SUM(amount) AS total FROM sales GROUP BY city ORDER BY total DESC, city;
SELECT rep, city, SUM(amount) FROM sales GROUP BY rep, city;
SELECT city, SUM(amount) FROM sales GROUP BY city HAVING SUM(amount) > 200 AND COUNT(*) > 1;
SELECT city, MAX(amount) - MIN(amount) AS spread FROM sales GROUP BY city ORDER BY spread DESC LIMIT 2;
SELECT city, COUNT(*) FROM sales GROUP BY city ORDER BY COUNT(*) DESC, city LIMIT 1 OFFSET 1;
SELECT amount / 100 AS bucket, COUNT(*) FROM sales GROUP BY amount / 100;
SELECT COUNT(*) FROM sales WHERE amount > 1000;
SELECT SUM(amount) FROM sales WHERE id > 100;
SELECT id, amount * 2 AS doubled FROM sales ORDER BY doubled DESC, id LIMIT 3;
EXPLAIN SELECT city, SUM(amount) FROM sales WHERE id > 1 GROUP BY city HAVING COUNT(*) > 1 ORDER BY city;
-- Errors
SELECT city, amount FROM sales GROUP BY city;
SELECT * FROM sales GROUP BY city;
SELECT city FROM sales WHERE SUM(amount) > 1;
SELECT MAX(amount) FROM sales WHERE id > 99;
SELECT SUM(city) FROM sales;
SELECT FOO(amount) FROM sales;
SELECT SUM(SUM(amount)) FROM sales;
UPDATE sales SET amount = COUNT(*);
