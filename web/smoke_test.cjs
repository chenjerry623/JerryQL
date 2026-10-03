// Checks that the WebAssembly build produces the same output as the native
// shell on the demo script (tests/sql/demo.expected).
//
//   node web/smoke_test.cjs build-web/jerryql.js
const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const createJerryQL = require(path.resolve(process.argv[2] || 'build-web/jerryql.js'));

function runSql(engine, sql) {
  const ptr = engine.ccall('jerryql_run', 'number', ['string'], [sql]);
  const text = engine.UTF8ToString(ptr);
  engine.ccall('jerryql_free', null, ['number'], [ptr]);
  return text;
}

createJerryQL().then(function checkDemo(engine) {
  const demo = fs.readFileSync(path.join(root, 'examples/demo.sql'), 'utf8');
  const expected = fs.readFileSync(path.join(root, 'tests/sql/demo.expected'), 'utf8');
  const actual = runSql(engine, demo);
  if (actual !== expected) {
    console.error('WebAssembly output differs from tests/sql/demo.expected:\n' + actual);
    process.exit(1);
  }
  console.log('WebAssembly build matches the native golden output.');
});
