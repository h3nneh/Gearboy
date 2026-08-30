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

// End to end gate for the whole live view feature. Starts the built emulator
// headless with the MCP server and the live view server on one process, then
// checks the stream, the served viewer page and the status channel that the
// agent tools feed. The result is the exit code. Node >= 22, no dependencies.
//
// Usage: node tests/e2e/liveview_full_probe.mjs
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
const STATUS_TIMEOUT_MS = 2000;
const SHUTDOWN_MS = 3000;
const STARTUP_TIMEOUT_MS = 20000;
const PROTOCOL_VERSION = '2025-11-25';
const BUTTONS = ['a', 'b', 'start', 'select', 'up', 'down', 'left', 'right'];
const WATCH_ADDRESS = 0xC000;
const WATCH_LABEL = 'full_probe_watch';

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

// A minimal MCP client for the streamable http transport: one request per
// connection, the protocol version echoed back after the handshake.
class McpClient
{
    constructor(port)
    {
        this.port = port;
        this.id = 0;
        this.version = null;
    }

    async request(method, params)
    {
        const headers = { 'content-type': 'application/json' };

        if (this.version)
            headers['MCP-Protocol-Version'] = this.version;

        const response = await fetch(`http://127.0.0.1:${this.port}/mcp`, {
            method: 'POST',
            headers,
            body: JSON.stringify({ jsonrpc: '2.0', id: ++this.id, method, params })
        });

        const text = await response.text();
        return { status: response.status, body: text ? JSON.parse(text) : null };
    }

    async initialize()
    {
        const response = await this.request('initialize', {
            protocolVersion: PROTOCOL_VERSION,
            capabilities: {},
            clientInfo: { name: 'liveview-full-probe', version: '1' }
        });

        this.version = response.body?.result?.protocolVersion || null;
        return response;
    }

    async call(name, args)
    {
        const response = await this.request('tools/call', { name, arguments: args });
        const text = response.body?.result?.content?.[0]?.text;

        return {
            status: response.status,
            error: response.body?.error || null,
            isError: response.body?.result?.isError === true,
            payload: text ? JSON.parse(text) : null
        };
    }
}

// Collects everything the live view sends: binary frames counted and checked
// for the png signature, status frames kept for the assertions.
class LiveviewFeed
{
    constructor(port)
    {
        this.binary = 0;
        this.badSignature = 0;
        this.statuses = [];
        this.malformed = 0;
        this.error = null;
        this.socket = new WebSocket(`ws://127.0.0.1:${port}/ws`);
        this.socket.binaryType = 'arraybuffer';

        this.opened = new Promise((done) => {
            const timer = setTimeout(() => done(false), 10000);
            this.socket.addEventListener('open', () => {
                clearTimeout(timer);
                done(true);
            });
            this.socket.addEventListener('error', () => {
                this.error = 'the websocket connection failed';
                clearTimeout(timer);
                done(false);
            });
        });

        this.socket.addEventListener('message', (event) => {
            if (typeof event.data === 'string')
            {
                const status = parseJson(event.data);

                if (status === null)
                    this.malformed++;
                else
                    this.statuses.push(status);

                return;
            }

            const bytes = new Uint8Array(event.data);
            this.binary++;

            for (let i = 0; i < PNG_SIGNATURE.length; i++)
            {
                if (bytes[i] !== PNG_SIGNATURE[i])
                {
                    this.badSignature++;
                    return;
                }
            }
        });
    }

    // Waits for a status frame that arrives after `from` and matches.
    async waitForStatus(from, predicate, timeoutMs)
    {
        const deadline = Date.now() + timeoutMs;
        let index = from;

        while (Date.now() < deadline)
        {
            while (index < this.statuses.length)
            {
                const status = this.statuses[index++];

                if (predicate(status))
                    return status;
            }

            await sleep(25);
        }

        return null;
    }

    mark()
    {
        return this.statuses.length;
    }

    close()
    {
        this.socket.close();
    }
}

function parseJson(text)
{
    try
    {
        return JSON.parse(text);
    }
    catch
    {
        return null;
    }
}

function checkPage(body, contentType, status)
{
    console.log('viewer page');

    check(status === 200, `GET / answers 200 (got ${status})`);
    check((contentType || '').includes('text/html'), `GET / answers html (got ${contentType})`);
    check(/<canvas[\s>]/i.test(body), 'the page carries a canvas element');
    check(/id="agent-log"/.test(body), 'the page carries the agent text anchor');
    check(/id="watch-table"/.test(body), 'the page carries the watch table anchor');
    check(/id="input-state"/.test(body), 'the page carries the input display anchor');

    const external = body.match(/https?:\/\/[^\s"'<>]*/g) || [];
    check(external.length === 0,
        `the page references no external url${external.length > 0 ? ` (found ${external.join(', ')})` : ''}`);
}

function checkStatusShape(status)
{
    check(status.type === 'status', `status frames carry the status type (got ${status.type})`);
    check(typeof status.seq === 'number' && status.seq >= 0, `seq is a number (got ${status.seq})`);
    check(typeof status.agent_ts === 'number', `agent_ts is a number (got ${status.agent_ts})`);
    check(Array.isArray(status.watches), 'watches is an array');

    const media = status.media;
    check(!!media && typeof media.title === 'string' && typeof media.paused === 'boolean' &&
        media.width > 0 && media.height > 0, `media block is complete (got ${JSON.stringify(media)})`);

    const inputs = status.inputs;
    const complete = !!inputs && BUTTONS.every((name) => typeof inputs[name] === 'boolean');
    check(complete, `inputs block carries all eight buttons (got ${JSON.stringify(inputs)})`);
}

async function findWramArea(mcp)
{
    const areas = await mcp.call('list_memory_areas', {});
    const list = areas.payload?.areas || [];
    const wram = list.find((area) => area.name === 'WRAM');

    return wram ? wram.id : null;
}

async function run()
{
    const mcpPort = await freePort();
    const viewPort = await freePort();
    const child = startEmulator(['--headless',
        '--mcp-http', '--mcp-http-port', String(mcpPort),
        '--live-view', '--live-view-port', String(viewPort), ROM]);
    let exited = false;

    try
    {
        console.log('startup');

        if (!check(await waitForHttp(viewPort, STARTUP_TIMEOUT_MS), 'the live view server accepts requests'))
            return;

        const root = await fetch(`http://127.0.0.1:${viewPort}/`);
        const body = await root.text();
        checkPage(body, root.headers.get('content-type'), root.status);

        console.log('mcp handshake');

        const mcp = new McpClient(mcpPort);
        const initialize = await mcp.initialize();
        if (!check(initialize.status === 200 && mcp.version !== null,
            `initialize succeeds (status ${initialize.status}, version ${mcp.version})`))
            return;

        const tools = await mcp.request('tools/list', {});
        const names = (tools.body?.result?.tools || []).map((tool) => tool.name);
        check(names.includes('set_agent_status'), `tools/list contains set_agent_status (${names.length} tools)`);

        console.log('stream');

        const feed = new LiveviewFeed(viewPort);
        if (!check(await feed.opened, `the websocket on /ws opens (${feed.error || 'no error'})`))
            return;

        await sleep(MEASURE_MS);

        check(feed.binary >= MIN_FRAMES,
            `at least ${MIN_FRAMES} binary frames within ${MEASURE_MS} ms (got ${feed.binary})`);
        check(feed.badSignature === 0,
            `every binary frame starts with the png signature (${feed.badSignature} did not)`);
        check(feed.malformed === 0, `every status frame is json (${feed.malformed} were not)`);

        console.log('agent status');

        const firstMarker = `full probe alpha ${Date.now()}`;
        let mark = feed.mark();
        const firstCall = await mcp.call('set_agent_status', { text: firstMarker });
        check(!firstCall.isError && firstCall.payload?.success === true,
            `set_agent_status confirms the post (${JSON.stringify(firstCall.payload)})`);

        const firstStatus = await feed.waitForStatus(mark,
            (status) => status.agent_text === firstMarker, STATUS_TIMEOUT_MS);
        if (!check(!!firstStatus,
            `a status frame carries the posted text within ${STATUS_TIMEOUT_MS} ms`))
            return;

        checkStatusShape(firstStatus);

        const secondMarker = `full probe beta ${Date.now()}`;
        mark = feed.mark();
        await mcp.call('set_agent_status', { text: secondMarker });
        const secondStatus = await feed.waitForStatus(mark,
            (status) => status.agent_text === secondMarker, STATUS_TIMEOUT_MS);

        check(!!secondStatus && secondStatus.seq > firstStatus.seq,
            `a second post grows seq strictly (${firstStatus.seq} -> ${secondStatus?.seq})`);

        const missing = await mcp.request('tools/call', { name: 'set_agent_status', arguments: {} });
        check(!!missing.body?.error, `a call without text answers an error (${JSON.stringify(missing.body?.error)})`);
        check(child.exitCode === null && child.signalCode === null, 'the emulator survives the rejected call');

        console.log('memory watch');

        const area = await findWramArea(mcp);
        if (!check(area !== null, 'list_memory_areas reports the WRAM area'))
            return;

        mark = feed.mark();
        const added = await mcp.call('add_memory_watch', {
            area,
            address: WATCH_ADDRESS.toString(16).toUpperCase(),
            notes: WATCH_LABEL,
            size: 8
        });
        check(added.payload?.success === true, `add_memory_watch succeeds (${JSON.stringify(added.payload)})`);

        const watchStatus = await feed.waitForStatus(mark,
            (status) => status.watches.some((watch) => watch.label === WATCH_LABEL), STATUS_TIMEOUT_MS);
        const watch = watchStatus?.watches?.find((entry) => entry.label === WATCH_LABEL);

        if (check(!!watch, `a status frame carries the watch within ${STATUS_TIMEOUT_MS} ms`))
        {
            check(watch.address === WATCH_ADDRESS,
                `the watch reports its address (got ${watch.address})`);
            check(watch.size === 1, `the watch reports its size in bytes (got ${watch.size})`);
            check(Number.isInteger(watch.value) && watch.value >= 0 && watch.value <= 0xFF,
                `the watch reports a byte value (got ${watch.value})`);
        }

        console.log('input');

        mark = feed.mark();
        const pressed = await mcp.call('controller_button', { player: 1, button: 'a', action: 'press' });
        check(!pressed.isError, `controller_button presses a (${JSON.stringify(pressed.payload)})`);

        const inputStatus = await feed.waitForStatus(mark,
            (status) => status.inputs.a === true, STATUS_TIMEOUT_MS);
        check(!!inputStatus, `a status frame reports the pressed button within ${STATUS_TIMEOUT_MS} ms`);

        console.log('shutdown');

        feed.close();

        const start = Date.now();
        child.kill('SIGTERM');
        exited = await waitForExit(child, SHUTDOWN_MS);
        check(exited, `the emulator exits within ${SHUTDOWN_MS} ms after SIGTERM (took ${Date.now() - start} ms)`);
        check(child.exitCode === 0, `the emulator exits cleanly (code ${child.exitCode}, signal ${child.signalCode})`);
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

    await run();

    if (failed > 0)
    {
        console.log(`\nliveview_full_probe: ${failed} check(s) failed`);
        return 1;
    }

    console.log('\nliveview_full_probe: all checks passed');
    return 0;
}

process.exitCode = await main();
