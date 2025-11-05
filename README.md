# JerryQL

**JerryQL** is a lightweight, in-memory SQL-like database built in **C++17**.  
It supports creating tables, inserting data, selecting, updating, deleting rows, and saving data to disk.

---

## Setup

### Requirements
- C++17-compatible compiler (`g++`)
- (Windows) Install [MSYS2](https://www.msys2.org/) or MinGW and ensure `g++` is available in PATH.

### Build
g++ -std=c++17 -O2 -Wall -Wextra -pedantic -o jerryql main.cpp

### Run
./jerryql

You'll see:
JerryQL - SQL-like CLI Database
Loaded: data.db
Type HELP for a list of commands.

## Commands

| Command | Description | Example |
|----------|--------------|----------|
| `CREATE TABLE <name> <col1> <col2> ...` | Create a new table | `CREATE TABLE users name age city` |
| `INSERT <table> <val1> <val2> ...` | Insert a new row | `INSERT users Alice 24 London` |
| `SELECT <cols|*> FROM <table> [WHERE <col>=<val>]` | Query data | `SELECT name FROM users WHERE city=London` |
| `UPDATE <table> SET <col>=<val> [WHERE <col>=<val>]` | Modify rows | `UPDATE users SET city=Paris WHERE name=Alice` |
| `DELETE FROM <table> [WHERE <col>=<val>]` | Remove rows | `DELETE FROM users WHERE name=Alice` |
| `DESCRIBE <table>` | Show table schema and row count | `DESCRIBE users` |
| `SHOW <table>` | Display table contents | `SHOW users` |
| `DROP TABLE <name>` | Delete a table | `DROP TABLE users` |
| `MEMORY` / `STATS` | Show memory usage and table count | `STATS` |
| `HELP` | Display help menu | `HELP` |
| `EXIT` | Save database and exit | `EXIT` |

# Example Session

> CREATE TABLE employees id name department  
OK.  

> INSERT employees 1 John Sales  
OK.  

> INSERT employees 2 Jane HR  
OK.  

> SELECT * FROM employees  
---------------------------  
id | name | department |  
---------------------------  
1  | John | Sales      |  
2  | Jane | HR         |  
---------------------------  

> UPDATE employees SET department=Finance WHERE id=2  
OK - 1 row(s) updated.  

> SELECT name,department FROM employees WHERE department=Finance  
---------------------------  
name | department |  
---------------------------  
Jane | Finance    |  
---------------------------  

> DELETE FROM employees WHERE id=1  
OK - 1 row(s) deleted.  

> SHOW employees  
---------------------------  
id | name | department |  
---------------------------  
2  | Jane | Finance    |  
---------------------------  

> STATS  
Tables: 1  
Memory: 320 bytes  

> EXIT  
Bye.  
