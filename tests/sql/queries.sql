-- SELECT features end to end.
CREATE TABLE people (id INT PRIMARY KEY, name TEXT, age INT, city TEXT);
INSERT INTO people VALUES (3, 'Cleo', 41, 'Vancouver'), (1, 'Ada', 36, 'London'),
  (4, 'Dan', 29, 'Vancouver'), (2, 'Brian', 29, 'Toronto'), (5, 'Eve', 52, 'London');
-- Rows come back in primary-key order regardless of insert order.
SELECT * FROM people;
select NAME, Age from PEOPLE where AGE = 29;
SELECT name, age * 12 AS months, age > 40 AS senior FROM people ORDER BY age DESC, name;
SELECT name FROM people WHERE city = 'Vancouver' AND NOT age < 30;
SELECT name FROM people WHERE (city = 'London' OR city = 'Toronto') AND age <> 52;
SELECT id FROM people WHERE id > 2;
SELECT id FROM people WHERE 2 >= id;
SELECT id FROM people WHERE id > 3 AND id < 3;
SELECT name FROM people ORDER BY city, name DESC LIMIT 4;
SELECT name, 'it''s' AS quote, -id FROM people WHERE id = -(-1);
EXPLAIN SELECT * FROM people WHERE id = 4;
EXPLAIN SELECT name FROM people WHERE id < 3 OR age > 40 ORDER BY name LIMIT 2;
