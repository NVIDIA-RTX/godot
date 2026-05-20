import test, { ExecutionContext } from 'ava';
import { promises as fs } from 'fs';
import { resolve } from 'path';
import { pathRegex, envRegex } from '../src/matchers';

const powershellLogPath = 'fixtures/construct_env/powershell.log';
const ubuntuBashLogPath = 'fixtures/construct_env/ubuntu-bash.log';
const macosBashLogPath = 'fixtures/construct_env/macos-bash.log';

function trimTimestamp(line: string) {
  return line.slice(29)
}

async function matchLog(t: ExecutionContext, matcher: RegExp, filename: string) {
  const path = resolve(__dirname, filename);
  const log = await fs.readFile(path, 'utf-8');
  const results: Array<any> = [];
  for (const line of log.split('\n').map(trimTimestamp)) {
    const [, ...captures] = matcher.exec(line) || [];
    results.push({ input: line, captures });
  }
  t.snapshot(results);
}

test('pathRegex matches powershell output', matchLog, pathRegex, powershellLogPath);
test('pathRegex matches ubuntu bash output', matchLog, pathRegex, ubuntuBashLogPath);
test('pathRegex matches macos bash output', matchLog, pathRegex, macosBashLogPath);
test('envRegex matches powershell output', matchLog, envRegex, powershellLogPath);
test('envRegex matches ubuntu bash output', matchLog, envRegex, ubuntuBashLogPath);
test('envRegex matches macos bash output', matchLog, envRegex, macosBashLogPath);
