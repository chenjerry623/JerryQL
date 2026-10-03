-- INSERT / UPDATE / DELETE / DROP end to end.
CREATE TABLE accounts (id INT PRIMARY KEY, owner TEXT, balance INT);
INSERT INTO accounts (owner, balance, id) VALUES ('ann', 100, 10), ('bob', 50, 20), ('cat', 75, 30);
UPDATE accounts SET balance = balance - 25 WHERE owner = 'ann';
UPDATE accounts SET id = id + 1;
SELECT * FROM accounts;
UPDATE accounts SET id = 21 WHERE id = 11;
UPDATE accounts SET balance = balance * 2 WHERE id >= 21 AND id <= 31;
DELETE FROM accounts WHERE id = 21;
SELECT * FROM accounts;
DELETE FROM accounts;
SELECT * FROM accounts;
CREATE TABLE notes (body TEXT);
INSERT INTO notes VALUES ('second'), ('first');
SELECT * FROM notes;
DROP TABLE notes;
SELECT * FROM notes;
