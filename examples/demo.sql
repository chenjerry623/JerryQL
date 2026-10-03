-- JerryQL demo: run with  ./build/jerryql --echo examples/demo.sql
CREATE TABLE employees (id INT PRIMARY KEY, name TEXT, dept TEXT, salary INT);

INSERT INTO employees VALUES
  (1, 'Ada', 'Engineering', 185000),
  (2, 'Grace', 'Engineering', 172000),
  (3, 'Linus', 'Infra', 158000),
  (4, 'Barbara', 'Research', 191000),
  (5, 'Ken', 'Infra', 149000);

SELECT * FROM employees;

-- Filters, expressions, ordering and limits
SELECT name, salary / 12 AS monthly FROM employees
WHERE dept = 'Engineering' OR salary > 190000
ORDER BY salary DESC;

SELECT name FROM employees ORDER BY name LIMIT 3;

-- The planner uses the primary key instead of scanning the whole table
EXPLAIN SELECT name FROM employees WHERE id = 3;
EXPLAIN SELECT name FROM employees WHERE id >= 2 AND id < 5;
EXPLAIN SELECT name FROM employees WHERE dept = 'Infra' ORDER BY salary DESC LIMIT 1;

-- Writes
UPDATE employees SET salary = salary + 10000 WHERE dept = 'Infra';
DELETE FROM employees WHERE id = 2;
SELECT id, name, salary FROM employees WHERE id >= 2 AND id <= 5;

-- Errors are reported without stopping the script
INSERT INTO employees VALUES (1, 'Duplicate', 'X', 0);
SELECT name FROM employees WHERE salary = 'lots';
SELEC * FROM employees;
