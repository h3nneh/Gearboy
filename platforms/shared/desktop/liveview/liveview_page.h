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

#ifndef LIVEVIEW_PAGE_H
#define LIVEVIEW_PAGE_H

// The viewer page served on GET /. Everything it needs is in this string:
// no external resource, no script, style or font from anywhere else (spec D5).
// It speaks the live view protocol of spec D1 to D3: png images on binary
// frames, status json on text frames.

static const char* const k_liveview_page =
R"LIVEVIEWPAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Gearboy Live View</title>
<style>
:root {
    color-scheme: dark;
    --bg: #16181d;
    --panel: #1e2229;
    --line: #2c313b;
    --text: #d9dee7;
    --dim: #8b95a6;
    --accent: #9ad14b;
    --warn: #d9a441;
    --bad: #d1584b;
}
* { box-sizing: border-box; }
body {
    margin: 0;
    background: var(--bg);
    color: var(--text);
    font-family: ui-monospace, monospace;
    font-size: 13px;
}
header {
    display: flex;
    align-items: baseline;
    gap: 12px;
    padding: 10px 16px;
    border-bottom: 1px solid var(--line);
}
h1 { font-size: 15px; margin: 0; font-weight: 600; }
#media-title { color: var(--dim); }
#connection {
    margin-left: auto;
    padding: 2px 8px;
    border: 1px solid var(--line);
    border-radius: 10px;
    color: var(--dim);
}
#connection[data-state="online"] { color: var(--accent); border-color: var(--accent); }
#connection[data-state="connecting"] { color: var(--warn); border-color: var(--warn); }
#connection[data-state="offline"] { color: var(--bad); border-color: var(--bad); }
main {
    display: grid;
    grid-template-columns: minmax(320px, 2fr) minmax(280px, 1fr);
    gap: 16px;
    padding: 16px;
    align-items: start;
}
@media (max-width: 900px) { main { grid-template-columns: 1fr; } }
section {
    background: var(--panel);
    border: 1px solid var(--line);
    border-radius: 6px;
    padding: 12px;
}
h2 {
    font-size: 11px;
    letter-spacing: 0.1em;
    text-transform: uppercase;
    color: var(--dim);
    margin: 0 0 8px;
    font-weight: 600;
}
#screen {
    display: block;
    width: 100%;
    height: auto;
    background: #000;
    border-radius: 4px;
    image-rendering: pixelated;
    image-rendering: crisp-edges;
}
#screen-size { color: var(--dim); margin-top: 8px; }
.side { display: grid; gap: 16px; }
#input-state { display: flex; flex-wrap: wrap; gap: 6px; }
.button {
    border: 1px solid var(--line);
    border-radius: 4px;
    padding: 3px 8px;
    color: var(--dim);
    min-width: 46px;
    text-align: center;
}
.button[data-pressed="true"] {
    color: #16181d;
    background: var(--accent);
    border-color: var(--accent);
}
table { border-collapse: collapse; width: 100%; }
th, td { text-align: left; padding: 3px 6px; border-bottom: 1px solid var(--line); }
th { color: var(--dim); font-weight: 600; }
td.num { text-align: right; }
.empty { color: var(--dim); padding: 4px 6px; }
#agent-log { display: grid; gap: 8px; max-height: 460px; overflow-y: auto; }
.entry { border-left: 2px solid var(--accent); padding-left: 8px; }
.entry .meta { color: var(--dim); font-size: 11px; }
.entry .text { white-space: pre-wrap; overflow-wrap: anywhere; margin-top: 2px; }
.entry.old { border-left-color: var(--line); }
</style>
</head>
<body>
<header>
<h1>Gearboy Live View</h1>
<span id="media-title"></span>
<span id="connection" data-state="connecting">connecting</span>
</header>
<main>
<section>
<h2>Screen</h2>
<canvas id="screen" width="160" height="144"></canvas>
<div id="screen-size">no frame yet</div>
</section>
<div class="side">
<section>
<h2>Agent status</h2>
<div id="agent-log"><div class="empty">no status posted yet</div></div>
</section>
<section>
<h2>Watches</h2>
<table id="watch-table">
<thead><tr><th>Label</th><th>Address</th><th class="num">Value</th></tr></thead>
<tbody><tr><td class="empty" colspan="3">no watches registered</td></tr></tbody>
</table>
</section>
<section>
<h2>Input</h2>
<div id="input-state"></div>
</section>
</div>
</main>
<script>
(function () {
    var BUTTONS = ['a', 'b', 'start', 'select', 'up', 'down', 'left', 'right'];
    var HISTORY_MAX = 50;
    var BACKOFF_MIN_MS = 250;
    var BACKOFF_MAX_MS = 5000;

    var canvas = document.getElementById('screen');
    var context = canvas.getContext('2d');
    var screenSize = document.getElementById('screen-size');
    var mediaTitle = document.getElementById('media-title');
    var connection = document.getElementById('connection');
    var agentLog = document.getElementById('agent-log');
    var watchBody = document.querySelector('#watch-table tbody');
    var inputState = document.getElementById('input-state');

    var indicators = {};
    BUTTONS.forEach(function (name) {
        var element = document.createElement('span');
        element.className = 'button';
        element.textContent = name;
        element.dataset.pressed = 'false';
        inputState.appendChild(element);
        indicators[name] = element;
    });

    var socket = null;
    var backoff = BACKOFF_MIN_MS;
    var lastAgentSeq = -1;
    var frames = 0;

    function setConnection(state, text) {
        connection.dataset.state = state;
        connection.textContent = text;
    }

    function hex(value, digits) {
        var text = (value >>> 0).toString(16).toUpperCase();
        while (text.length < digits)
            text = '0' + text;
        return text;
    }

    function drawFrame(blob) {
        var url = URL.createObjectURL(blob);
        var image = new Image();

        image.onload = function () {
            context.imageSmoothingEnabled = false;
            context.drawImage(image, 0, 0, canvas.width, canvas.height);
            URL.revokeObjectURL(url);
        };

        image.onerror = function () {
            URL.revokeObjectURL(url);
        };

        image.src = url;
    }

    function applyMedia(media) {
        if (!media)
            return;

        var width = media.width | 0;
        var height = media.height | 0;

        if (width > 0 && height > 0 && (canvas.width !== width || canvas.height !== height)) {
            canvas.width = width;
            canvas.height = height;
            context.imageSmoothingEnabled = false;
        }

        mediaTitle.textContent = media.paused ? media.title + ' (paused)' : media.title;
    }

    function applyAgent(status) {
        var text = typeof status.agent_text === 'string' ? status.agent_text : '';

        if (text.length === 0 || status.seq === lastAgentSeq)
            return;

        lastAgentSeq = status.seq;

        var stamp = status.agent_ts > 0 ? new Date(status.agent_ts).toLocaleTimeString() : '';
        var entry = document.createElement('div');
        entry.className = 'entry';

        var meta = document.createElement('div');
        meta.className = 'meta';
        meta.textContent = '#' + status.seq + (stamp ? ' - ' + stamp : '');

        var body = document.createElement('div');
        body.className = 'text';
        body.textContent = text;

        entry.appendChild(meta);
        entry.appendChild(body);

        var placeholder = agentLog.querySelector('.empty');
        if (placeholder)
            agentLog.removeChild(placeholder);

        Array.prototype.forEach.call(agentLog.children, function (child) {
            child.classList.add('old');
        });

        agentLog.insertBefore(entry, agentLog.firstChild);

        while (agentLog.children.length > HISTORY_MAX)
            agentLog.removeChild(agentLog.lastChild);
    }

    function applyWatches(watches) {
        watchBody.textContent = '';

        if (!watches || watches.length === 0) {
            var empty = document.createElement('tr');
            var cell = document.createElement('td');
            cell.className = 'empty';
            cell.colSpan = 3;
            cell.textContent = 'no watches registered';
            empty.appendChild(cell);
            watchBody.appendChild(empty);
            return;
        }

        watches.forEach(function (watch) {
            var row = document.createElement('tr');

            var label = document.createElement('td');
            label.textContent = watch.label;

            var address = document.createElement('td');
            address.textContent = '$' + hex(watch.address, 4);

            var value = document.createElement('td');
            value.className = 'num';
            value.textContent = watch.value + ' ($' + hex(watch.value, watch.size * 2) + ')';

            row.appendChild(label);
            row.appendChild(address);
            row.appendChild(value);
            watchBody.appendChild(row);
        });
    }

    function applyInputs(inputs) {
        BUTTONS.forEach(function (name) {
            var pressed = !!(inputs && inputs[name]);
            indicators[name].dataset.pressed = pressed ? 'true' : 'false';
        });
    }

    function applyStatus(status) {
        if (!status || status.type !== 'status')
            return;

        applyMedia(status.media);
        applyAgent(status);
        applyWatches(status.watches);
        applyInputs(status.inputs);
    }

    function connect() {
        setConnection('connecting', 'connecting');
        socket = new WebSocket('ws://' + window.location.host + '/ws');
        socket.binaryType = 'blob';

        socket.addEventListener('open', function () {
            backoff = BACKOFF_MIN_MS;
            setConnection('online', 'connected');
        });

        socket.addEventListener('message', function (event) {
            if (typeof event.data === 'string') {
                applyStatus(JSON.parse(event.data));
                return;
            }

            frames++;
            screenSize.textContent = canvas.width + ' x ' + canvas.height + ', ' + frames + ' frames';
            drawFrame(event.data);
        });

        socket.addEventListener('close', function () {
            setConnection('offline', 'disconnected, reconnecting in ' + (backoff / 1000).toFixed(1) + ' s');
            window.setTimeout(connect, backoff);
            backoff = Math.min(backoff * 2, BACKOFF_MAX_MS);
        });

        socket.addEventListener('error', function () {
            if (socket.readyState !== WebSocket.CLOSED)
                socket.close();
        });
    }

    applyInputs(null);
    connect();
})();
</script>
</body>
</html>
)LIVEVIEWPAGE";

#endif /* LIVEVIEW_PAGE_H */
