#!/usr/bin/env node
/*
 * Gearboy - Nintendo Game Boy Emulator
 * Copyright (C) 2012  Ignacio Sanchez

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see http://www.gnu.org/licenses/
 *
 */

// End to end probe for the live view stream. Starts the built emulator
// headless with the live view server, measures the stream and reports the
// result as an exit code. Node >= 22, no dependencies.
//
// Usage: node tests/e2e/liveview_stream_probe.mjs
// The ROM comes from GEARBOY_TEST_ROM and is never part of the repository.

import { spawn } from 'node:child_process';
import net from 'node:net';
import { existsSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const REPO_ROOT = resolve(dirname(fileURLToPath(import.meta.url)), '..', '..');
const BINARY = resolve(REPO_ROOT, 'platforms', 'linux', 'gearboy');
const ROM = process.env.GEARBOY_TEST_ROM ||
    '/workspace/python/pokered_planner/pokemon_red.gb';
const PNG_SIGNATURE = [0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a];
const MEASURE_MS = 5000;
const MIN_FRAMES = 40;
const SHUTDOWN_MS = 3000;
const STARTUP_TIMEOUT_MS = 20000;

let failed = 0;

function check(condition, message)
{
    if (condition)
    {
        console.log(`  ok   ${message}`);
        return true;
    }

    console.log(`  FAIL ${message}`);
    failed++;
    return false;
}

function sleep(ms)
{
    return new Promise((done) => setTimeout(done, ms));
}

function freePort()
{
    return new Promise((done, fail) => {
        const probe = net.createServer();
        probe.once('error', fail);
        probe.listen(0, '127.0.0.1', () => {
            const port = probe.address().port;
            probe.close(() => done(port));
        });
    });
}

// Resolves true when nothing accepts a connection on the port.
function connectionRefused(port)
{
    return new Promise((done) => {
        const socket = net.connect({ host: '127.0.0.1', port });
        const finish = (result) => {
            socket.destroy();
            done(result);
        };
        socket.setTimeout(1000);
        socket.once('connect', () => finish(false));
        socket.once('timeout', () => finish(false));
        socket.once('error', () => finish(true));
    });
}

function startEmulator(args)
{
    const child = spawn(BINARY, args, { cwd: REPO_ROOT, stdio: ['ignore', 'pipe', 'pipe'] });
    child.stdout.on('data', () => {});
    child.stderr.on('data', () => {});
    return child;
}

function waitForExit(child, timeoutMs)
{
    return new Promise((done) => {
        if (child.exitCode !== null || child.signalCode !== null)
        {
            done(true);
            return;
        }

        const timer = setTimeout(() => done(false), timeoutMs);
        child.once('exit', () => {
            clearTimeout(timer);
            done(true);
        });
    });
}

async function waitForHttp(port, timeoutMs)
{
    const deadline = Date.now() + timeoutMs;

    while (Date.now() < deadline)
    {
        try
        {
            const response = await fetch(`http://127.0.0.1:${port}/`);
            await response.arrayBuffer();
            return true;
        }
        catch
        {
            await sleep(200);
        }
    }

    return false;
}

// Collects binary messages for the measurement window.
function measureStream(port, windowMs)
{
    return new Promise((done) => {
        const result = { open: false, binary: 0, text: 0, badSignature: 0, error: null };
        const socket = new WebSocket(`ws://127.0.0.1:${port}/ws`);
        socket.binaryType = 'arraybuffer';

        const finish = () => {
            try
            {
                socket.close();
            }
            catch
            {
                // The socket is already gone, nothing to close.
            }
            done(result);
        };

        const openTimer = setTimeout(() => {
            result.error = 'the websocket did not open in time';
            finish();
        }, 10000);

        socket.addEventListener('open', () => {
            clearTimeout(openTimer);
            result.open = true;
            setTimeout(finish, windowMs);
        });

        socket.addEventListener('error', () => {
            if (!result.open)
            {
                clearTimeout(openTimer);
                result.error = 'the websocket connection failed';
                finish();
            }
        });

        socket.addEventListener('message', (event) => {
            if (typeof event.data === 'string')
            {
                result.text++;
                return;
            }

            const bytes = new Uint8Array(event.data);
            result.binary++;

            for (let i = 0; i < PNG_SIGNATURE.length; i++)
            {
                if (bytes[i] !== PNG_SIGNATURE[i])
                {
                    result.badSignature++;
                    return;
                }
            }
        });
    });
}

async function checkHelp()
{
    console.log('help text');

    const child = spawn(BINARY, ['--help'], { cwd: REPO_ROOT });
    let output = '';
    child.stdout.on('data', (chunk) => { output += chunk; });
    await waitForExit(child, 10000);

    check(output.includes('--live-view '), '--help lists --live-view');
    check(output.includes('--live-view-port'), '--help lists --live-view-port');
    check(output.includes('--live-view-address'), '--help lists --live-view-address');
}

async function checkNoServerWithoutFlag()
{
    console.log('without --live-view');

    const port = await freePort();
    const child = startEmulator(['--headless', '--live-view-port', String(port), ROM]);
    await sleep(2000);
    check(await connectionRefused(port), 'nothing listens on the live view port');
    child.kill('SIGTERM');
    await waitForExit(child, SHUTDOWN_MS);
}

async function checkStream()
{
    console.log('live view stream');

    const port = await freePort();
    const child = startEmulator(['--headless', '--live-view', '--live-view-port', String(port), ROM]);
    let exited = false;

    try
    {
        if (!check(await waitForHttp(port, STARTUP_TIMEOUT_MS), 'the live view server accepts requests'))
            return;

        const root = await fetch(`http://127.0.0.1:${port}/`);
        const body = await root.text();
        check(root.status === 200, `GET / answers 200 (got ${root.status})`);
        check((root.headers.get('content-type') || '').includes('text/html'),
            `GET / answers html (got ${root.headers.get('content-type')})`);
        check(body.length > 0, 'GET / answers with a page body');

        const unknown = await fetch(`http://127.0.0.1:${port}/nothing-here`);
        await unknown.text();
        check(unknown.status === 404, `an unknown path answers 404 (got ${unknown.status})`);

        const stream = await measureStream(port, MEASURE_MS);

        if (!check(stream.open, `the websocket on /ws opens (${stream.error || 'no error'})`))
            return;

        check(stream.binary >= MIN_FRAMES,
            `at least ${MIN_FRAMES} binary frames within ${MEASURE_MS} ms (got ${stream.binary})`);
        check(stream.badSignature === 0,
            `every binary frame starts with the png signature (${stream.badSignature} did not)`);

        const start = Date.now();
        child.kill('SIGTERM');
        exited = await waitForExit(child, SHUTDOWN_MS);
        check(exited, `the emulator exits within ${SHUTDOWN_MS} ms after SIGTERM (took ${Date.now() - start} ms)`);
    }
    finally
    {
        if (!exited)
        {
            child.kill('SIGKILL');
            await waitForExit(child, 2000);
        }
    }
}

async function main()
{
    if (!existsSync(BINARY))
    {
        console.error(`missing emulator binary: ${BINARY}`);
        console.error('build it first: make -C platforms/linux -j$(nproc)');
        return 2;
    }

    if (!existsSync(ROM))
    {
        console.error(`missing test rom: ${ROM}`);
        console.error('set GEARBOY_TEST_ROM to a Game Boy rom file');
        return 2;
    }

    await checkHelp();
    await checkNoServerWithoutFlag();
    await checkStream();

    if (failed > 0)
    {
        console.log(`\nliveview_stream_probe: ${failed} check(s) failed`);
        return 1;
    }

    console.log('\nliveview_stream_probe: all checks passed');
    return 0;
}

process.exitCode = await main();
