// Saadin heating graph page (/graph), drawn with d3 from /graph.json, see Manuals/Heating.md
// Served after d3 in /graph.js (see CMakeLists.txt). The first part works out what to draw. It uses no page and no d3,
// so it also runs as a host test under node (see test/graph_test.js).
var Graph = (function () {
    'use strict';
    var COLS = [
        'room',
        'outdoor',
        'heatw',
        'power',
        'target',
        'effective',
        'setpoint',
        'demand',
        'smoothed',
        'integral',
        'kp',
        'ti',
        'liquid',
        'comp',
        'fan',
        'intake',
        'heatout',
        'deficit',
        'heattarget',
        'scale',
        'fanset',
        'quiet'
    ];
    var SHADED = { defrost: 1, hold: 1, rest: 1, idle: 1, cold: 1, nosensor: 1 }; // Sub-states, then states

    function runmean(num, den, win, min) {
        // Mean of num over the last win samples, or sum(num)/sum(den). null with fewer than min samples
        var ps = [0],
            pd = [0],
            pn = [0];
        num.forEach(function (v, i) {
            var o = v != null && (!den || den[i] != null);
            ps.push(ps[i] + (o ? v : 0));
            pd.push(pd[i] + (o && den ? den[i] : 0));
            pn.push(pn[i] + (o ? 1 : 0));
        });
        return num.map(function (_, i) {
            var j = Math.max(0, i + 1 - win),
                c = pn[i + 1] - pn[j];
            if (c < min) return null;
            if (!den) return (ps[i + 1] - ps[j]) / c;
            var e = pd[i + 1] - pd[j];
            return e > 0 ? (ps[i + 1] - ps[j]) / e : null;
        });
    }

    function quantile(sorted, p) {
        var h = (sorted.length - 1) * p,
            i = Math.floor(h);
        return i + 1 < sorted.length ? sorted[i] + (sorted[i + 1] - sorted[i]) * (h - i) : sorted[i];
    }

    function prep(d, at, F) {
        // The log d, read at time at (ms), as series in the units shown (F for Fahrenheit)
        var k = d.ago.length,
            m = { d: d, k: k, iv: d.interval * 1000 },
            per = 3600 / d.interval,
            i;
        COLS.forEach(function (c) {
            if (!d[c]) d[c] = new Array(k).fill(null);
        });
        if (!d.sub) {
            d.sub = new Array(k).fill(0);
            d.subs = [''];
        }
        m.t = d.ago.map(function (a) {
            return at - a * 1000;
        });
        var st = (m.st = function (i) {
            return d.states[d.state[i]];
        });
        var sb = (m.sb = function (i) {
            return d.subs[d.sub[i]] || '';
        });
        function cold(i) {
            return st(i) == 'cold';
        }
        function tc(a) {
            return a.map(function (v) {
                return v == null ? null : F ? v * 0.018 + 32 : v / 100;
            });
        }
        // Demand level: the demand.table entry in use, by its name there ("95", "100q", "100")
        var tab = (m.tab = {});
        m.levels = [];
        (d.table || '').split(',').forEach(function (e) {
            var p = e.split(':');
            if (p.length != 2) return;
            tab[p[0]] = +p[1];
            m.levels.push(p[0]);
        });
        m.lev = d.demand.map(function (v, i) {
            if (v == null || st(i) != 'demand') return null;
            var key = v == 100 ? (d.quiet[i] == null ? null : d.quiet[i] ? '100q' : '100') : String(v);
            return key != null && key in tab ? key : null;
        });
        m.levW = m.lev.map(function (key, i) {
            return key != null ? tab[key] : st(i) == 'idle' ? 0 : null;
        });
        m.levH = m.levW.map(function (w, i) {
            return w === 0 ? 0 : w == null || d.scale[i] == null ? null : Math.round((w * d.scale[i]) / 100);
        });
        m.room = tc(
            d.smoothed.some(function (v) {
                return v != null;
            })
                ? d.smoothed
                : d.room
        );
        m.target = tc(d.target);
        m.effective = tc(d.effective);
        m.setpoint = tc(d.setpoint);
        m.outdoor = tc(d.outdoor);
        m.liquid = tc(d.liquid);
        m.intake = tc(d.intake);
        // The integral is watts of heat in demand control, a setpoint at full power
        m.setint = tc(
            d.integral.map(function (v, i) {
                return cold(i) ? v : null;
            })
        );
        m.integral = d.integral.map(function (v, i) {
            return cold(i) ? null : v;
        });
        m.prop = d.heatw.map(function (v, i) {
            return v == null || m.integral[i] == null ? null : v - m.integral[i];
        });
        m.hm = runmean(d.heatout, null, 2 * per, per / 2);
        m.cop = d.heatout.map(function (v, i) {
            return v == null || d.power[i] == null || d.power[i] < 100 || sb(i) == 'defrost' ? null : v / d.power[i];
        });
        m.cm = runmean(d.heatout, d.power, 6 * per, per / 2);
        m.scale = d.scale.map(function (v) {
            return v == null ? null : v / 100;
        });
        function runs(f) {
            // Runs of samples with the same f(i), from the start of the first sample to the end of the last
            var out = [],
                cur = null;
            for (var i = 0; i < k; i++) {
                var v = f(i);
                if (cur && cur.v === v) cur.b = m.t[i];
                else {
                    cur = v == null ? null : { a: m.t[i] - m.iv, b: m.t[i], v: v };
                    if (cur) out.push(cur);
                }
            }
            return out;
        }
        m.runs = runs(function (i) {
            return SHADED[sb(i)] ? sb(i) : SHADED[st(i)] ? st(i) : null;
        });
        m.levRuns = runs(function (i) {
            return m.lev[i];
        });
        m.tune = function (i) {
            var v = d.kp[i];
            if (v == null) return '';
            return (
                'kp ' +
                (cold(i) ? v / 100 + ' ℃/℃' : v + ' W/℃') +
                (d.ti[i] ? ', ti ' + d.ti[i] + ' min' : ', no integral')
            );
        };
        m.marks = []; // Changes of kp or ti, apart for demand control and full power
        var last = [];
        for (i = 0; i < k; i++) {
            var tu = m.tune(i),
                c = +cold(i);
            if (!tu) continue;
            if (last[c] && last[c] != tu) m.marks.push({ t: m.t[i] - m.iv, s: tu });
            last[c] = tu;
        }
        return m;
    }

    function levelPower(m, i0, i1) {
        // Power drawn at each demand level in samples i0 to i1: steady running only, two minutes into a level
        var d = m.d,
            by = {},
            settle = Math.round(120 / d.interval),
            i,
            j;
        for (i = i0; i <= i1; i++) {
            var lv = m.lev[i],
                ok = lv != null && d.power[i] != null;
            for (j = 0; ok && j <= settle; j++) ok = i - j >= 0 && m.lev[i - j] === lv && m.sb(i - j) == '';
            if (ok) (by[lv] = by[lv] || []).push(d.power[i]);
        }
        return m.levels
            .filter(function (key) {
                return by[key] && by[key].length >= settle;
            })
            .map(function (key) {
                var s = by[key].sort(function (a, b) {
                    return a - b;
                });
                return {
                    key: key,
                    table: m.tab[key],
                    n: s.length,
                    median: quantile(s, 0.5),
                    lo: quantile(s, 0.1),
                    hi: quantile(s, 0.9)
                };
            });
    }

    function defrostStarts(m, i0, i1) {
        var out = [];
        for (var i = Math.max(1, i0); i <= i1; i++) if (m.sb(i) == 'defrost' && m.sb(i - 1) != 'defrost') out.push(i);
        return out;
    }

    function cycles(m, starts) {
        // COP and outdoor temperature of each defrost cycle, from one defrost start to the next, the defrost counted in
        var d = m.d,
            out = [];
        for (var j = 0; j + 1 < starts.length; j++) {
            var sh = 0,
                sp = 0,
                so = 0,
                n = 0,
                no = 0;
            for (var i = starts[j]; i < starts[j + 1]; i++) {
                if (d.heatout[i] != null && d.power[i] != null) {
                    sh += d.heatout[i];
                    sp += d.power[i];
                    n++;
                }
                if (m.outdoor[i] != null) {
                    so += m.outdoor[i];
                    no++;
                }
            }
            if (n > (starts[j + 1] - starts[j]) / 2 && sp > 0 && no) out.push({ outdoor: so / no, cop: sh / sp });
        }
        return out;
    }

    function through(m, starts, before, after) {
        // Room temperature through each defrost, from where it stood when the defrost began: before to after (minutes)
        var per = 60 / m.d.interval,
            a0 = -before * per,
            a1 = after * per,
            lines = [],
            j;
        starts.forEach(function (s) {
            if (m.room[s] == null) return;
            var ln = [];
            for (j = a0; j <= a1; j++) {
                var v = s + j >= 0 && s + j < m.k ? m.room[s + j] : null;
                ln.push(v == null ? null : v - m.room[s]);
            }
            lines.push(ln);
        });
        var median = [],
            low = null;
        for (j = 0; j <= a1 - a0; j++) {
            var v = lines
                .map(function (l) {
                    return l[j];
                })
                .filter(function (v) {
                    return v != null;
                });
            v.sort(function (a, b) {
                return a - b;
            });
            median.push(v.length ? quantile(v, 0.5) : null);
            if (v.length && (!low || median[j] < low.v)) low = { j: j, min: (a0 + j) / per, v: median[j] };
        }
        return { per: per, a0: a0, lines: lines, median: median, low: low };
    }

    function path(t, v, step, x, y, i0, i1, width) {
        // SVG path through samples i0 to i1, x(t) and y(v) in pixels, broken at a null. A stepped line holds each value
        // back to the sample before. With more than two samples a pixel it draws the least and most of each pixel
        // column.
        var o = '',
            pen = false,
            i,
            px,
            py;
        if (i1 - i0 < 2 * width) {
            for (i = i0; i <= i1; i++) {
                if (v[i] == null) {
                    pen = false;
                    continue;
                }
                px = x(t[i]).toFixed(1);
                py = y(v[i]).toFixed(1);
                o += !pen ? 'M' + px + ',' + py : step ? 'V' + py + 'H' + px : 'L' + px + ',' + py;
                pen = true;
            }
            return o;
        }
        var b = null,
            f,
            la,
            mn,
            mx;
        function flush() {
            if (b == null) return;
            var yf = y(f).toFixed(1);
            o += !pen ? 'M' + b + ',' + yf : step ? 'V' + yf + 'H' + b : 'L' + b + ',' + yf;
            if (mn != mx) o += 'V' + y(mn).toFixed(1) + 'V' + y(mx).toFixed(1) + 'V' + y(la).toFixed(1);
            pen = true;
            b = null;
        }
        for (i = i0; i <= i1; i++) {
            if (v[i] == null) {
                flush();
                pen = false;
                continue;
            }
            px = Math.round(x(t[i]));
            if (px !== b) {
                flush();
                b = px;
                f = mn = mx = v[i];
            }
            la = v[i];
            if (la < mn) mn = la;
            if (la > mx) mx = la;
        }
        flush();
        return o;
    }

    return {
        prep: prep,
        levelPower: levelPower,
        defrostStarts: defrostStarts,
        cycles: cycles,
        through: through,
        path: path
    };
})();
if (typeof module != 'undefined') module.exports = Graph;

// The page: the page gives an empty <div id=graph> and F (1 for Fahrenheit)
if (typeof document != 'undefined' && typeof d3 != 'undefined')
    (function () {
        'use strict';
        var F = +window.F || 0,
            U = F ? '℉' : '℃';
        var C = {
            red: '#e34948',
            blue: '#2a78d6',
            orange: '#eb6834',
            aqua: '#1baf7a',
            yellow: '#eda100',
            green: '#008300',
            violet: '#6250d6',
            ink: '#0b0b0b',
            ink2: '#52514e',
            muted: '#898781',
            grid: '#e1e0d9',
            axis: '#c3c2b7'
        };
        var RAMP = [
            '#cde2fb',
            '#b7d3f6',
            '#9ec5f4',
            '#86b6ef',
            '#6da7ec',
            '#5598e7',
            '#3987e5',
            '#2a78d6',
            '#256abf',
            '#1c5cab',
            '#184f95',
            '#104281',
            '#0d366b'
        ]; // Level lane, lighter to darker for more watts
        var SHADE = {
            defrost: ['#bdf', 'Defrost'],
            hold: ['#cfc', 'Hold after a defrost'],
            rest: ['#fcc', 'Rest after a defrost loop'],
            idle: ['#e4e4e4', 'Heating off'],
            cold: ['#dcf', 'Cold weather'],
            nosensor: ['#fe9', 'No room reading']
        };
        var STATE = {
            off: 'Not running',
            idle: 'Heating off',
            demand: 'Demand control',
            cold: 'Cold weather',
            nosensor: 'No room reading'
        };
        var SUB = { defrost: 'defrosting', hold: 'hold after a defrost', rest: 'rest after a defrost loop' };
        var FONT = 'sans-serif',
            L = 46,
            R = 12,
            MINSPAN = 6e5; // Plot margins (px), least range shown (ms)
        var Q = new URLSearchParams(location.search),
            RF = +Q.get('refresh') || 0,
            SP = +Q.get('span') || 0;
        var M,
            W,
            pw,
            dom,
            T0,
            T1,
            I0,
            I1,
            H = null,
            OFF = {},
            strips = [],
            plots = [],
            raf = 0,
            anT = 0,
            rfT = 0,
            rzT = 0;
        var xFull,
            zoom,
            brush,
            ovB,
            lane,
            saved = false;
        if (RF < 0) RF = 0;
        if (RF && RF < 5) RF = 5;

        var root = d3.select('#graph');
        root.append('style').text(
            [
                '#graph{max-width:1000px;font-size:14px}',
                '#graph h2{font-size:15px;margin:0}',
                '#graph h3{font-size:13px;margin:0}',
                '#graph .keys{display:flex;flex-wrap:wrap;gap:4px 14px;font-size:12px;margin:6px 0}',
                '#graph .keys i{display:inline-block;width:14px;height:10px;border:1px solid #0002;margin-right:5px}',
                '#graph .bar{position:sticky;top:0;z-index:2;background:#fff;border-radius:6px;padding:5px 10px}',
                '#graph .bar{margin:6px 0;font-size:13px;box-shadow:0 1px 3px #0004;min-height:18px}',
                '#graph .bar{font-variant-numeric:tabular-nums}',
                '#graph .g{background:#fff;border-radius:6px;padding:8px 0 4px;margin-bottom:10px}',
                '#graph .gh{padding:0 12px 2px;display:flex;flex-wrap:wrap;gap:2px 10px;align-items:baseline}',
                '#graph .gh span{color:#52514e;font-size:12px}',
                '#graph .hd{display:flex;flex-wrap:wrap;gap:1px 12px;align-items:center;padding:4px 12px 0 46px}',
                '#graph .hd{font-size:12px;min-height:22px}',
                '#graph .un{color:#52514e;font-weight:bold}',
                '#graph .hint{color:#898781}',
                '#graph .li{font:inherit;color:#52514e;background:none;border:0;box-shadow:none;min-height:0}',
                '#graph .li{padding:1px 0}',
                '#graph .li{display:inline-flex;align-items:center;gap:5px;cursor:pointer}',
                '#graph .li[disabled]{cursor:default}',
                '#graph .li[aria-pressed=false]{opacity:.45;text-decoration:line-through}',
                '#graph .li b{color:#000;font-variant-numeric:tabular-nums;min-width:3ch}',
                '#graph svg{display:block}',
                '#graph svg.plot{touch-action:pan-y;cursor:crosshair}',
                '#graph .an{display:grid;grid-template-columns:repeat(auto-fit,minmax(270px,1fr))}',
                '#graph .an{gap:6px 14px;padding:4px 12px}',
                '#graph .an p{font-size:12px;color:#52514e;margin:1px 0 2px}',
                '#graph .an svg{width:100%;height:auto}',
                '#graph #zb button[aria-pressed=true]{font-weight:bold}'
            ].join('')
        );
        var msg = root.append('p').text('Loading…');
        var ctl = root.append('p'),
            zb = ctl.append('span').attr('id', 'zb').attr('hidden', true);
        zb.append('span').text('Show ');
        [
            [0, 'All'],
            [259200, '3 days'],
            [86400, '1 day'],
            [43200, '12 h'],
            [10800, '3 h']
        ].forEach(function (b) {
            zb.append('button').attr('data-s', b[0]).text(b[1]);
            zb.append('span').text(' ');
        });
        ctl.append('span').text(' Refresh ');
        var rf = ctl.append('select');
        [
            [0, 'Off'],
            [15, '15 s'],
            [30, '30 s'],
            [60, '1 min'],
            [300, '5 min']
        ].forEach(function (o) {
            rf.append('option').attr('value', o[0]).text(o[1]);
        });
        if (RF && [15, 30, 60, 300].indexOf(RF) < 0)
            rf.append('option')
                .attr('value', RF)
                .text(RF + ' s');
        rf.property('value', RF).on('change', function () {
            RF = +this.value;
            address();
            clearTimeout(rfT);
            if (RF && !saved) rfT = setTimeout(load, RF * 1000);
        });
        ctl.append('span').text(' Saved log ');
        ctl.append('input')
            .attr('type', 'file')
            .attr('accept', '.json,application/json')
            .on('change', function () {
                var f = this.files[0];
                if (!f) return;
                f.text()
                    .then(function (s) {
                        saved = true; // A saved log stays until the page is loaded again
                        clearTimeout(rfT);
                        show(JSON.parse(s), f.lastModified);
                    })
                    .catch(function (e) {
                        msg.text('Could not read the log: ' + e);
                    });
            });
        var keys = root.append('div').attr('class', 'keys');
        Object.keys(SHADE).forEach(function (k) {
            var s = keys.append('span');
            s.append('i').style('background', SHADE[k][0]);
            s.append('span').text(SHADE[k][1]);
        });
        var bar = root.append('div').attr('class', 'bar');
        var ovS = root.append('section').attr('class', 'g'),
            ovH = ovS.append('div').attr('class', 'hd');
        ovH.append('span').attr('class', 'un').text('Whole log, room temperature');
        ovH.append('span').attr('class', 'hint').text('drag across it to choose the range shown below');
        var ov = ovS.append('svg').attr('font-family', FONT);
        var charts = root.append('div');
        var anS = root.append('section').attr('class', 'g'),
            anH = anS.append('div').attr('class', 'gh');
        anH.append('h2').text('The range shown, summed up');
        anH.append('span').text('These follow the range chosen above.');
        var an = anS.append('div').attr('class', 'an');
        root.append('p').text(
            [
                'One sample every 15 s, kept until a restart.',
                'Every strip has one scale and all share the time axis, so a defrost lines up down the page.',
                'Move the pointer over a strip, or tap it, and every legend shows its value at that moment.',
                'Click a legend entry to show or hide it.',
                'Drag to pan, ctrl+scroll or pinch to zoom, double-click for the whole log.',
                'Refresh loads the log again at the chosen interval and keeps the range shown.',
                'A dashed line marks a change of kp or ti. In cold weather the integral is a setpoint.',
                'Heat request and its integral are watts of heat.',
                'Heat target is the heat request and the making up, over some hours, for what defrosts cost.',
                'Heat deficit is the heat target less the heat given so far: demand steps up to make it up.',
                'The demand level line is the heat the controller takes the level in use to give.',
                'Beside the power it is the level’s watts in demand.table.',
                'Heat output and COP are estimates from the fan speed, the unit’s own room reading and the liquid',
                'temperature.',
                'The 2 h mean heat output and the 6 h mean COP count the defrosts in.',
                'Saved log shows a /graph.json saved earlier in place of the device’s log.'
            ].join(' ')
        );

        function address() {
            // Keep the refresh and the range chosen with the buttons in the address, for a reload or a bookmark
            var q = (RF ? 'refresh=' + RF : '') + (RF && SP ? '&' : '') + (SP ? 'span=' + SP : '');
            history.replaceState(null, '', location.pathname + (q ? '?' + q : ''));
        }

        // ---- which strips, in which groups ----
        function S(name, v, c, o) {
            return Object.assign({ name: name, v: v, c: c, w: 2 }, o || {});
        }
        function groupsFor(m) {
            var d = m.d;
            var g = [
                {
                    title: 'Room',
                    sub: 'Is the room at the target, and what the weather is doing.',
                    strips: [
                        {
                            unit: U,
                            h: 220,
                            dp: 2,
                            series: [
                                S('Room', m.room, C.red),
                                S('Target', m.target, C.blue, { step: 1 }),
                                S('Effective target', m.effective, C.blue, { step: 1, dash: '5 4', w: 1.5 }),
                                S('Setpoint', m.setpoint, C.yellow, { step: 1, off: 1 }),
                                S('Setpoint integral', m.setint, C.yellow, { step: 1, dash: '5 4', w: 1.5, off: 1 })
                            ]
                        },
                        { unit: 'Outdoor, ' + U, h: 96, dp: 1, one: 1, series: [S('Outdoor', m.outdoor, C.blue)] }
                    ]
                },
                {
                    title: 'Heat wanted and given',
                    sub: 'Watts of heat. The demand level line is what the controller takes the level in use to give.',
                    strips: [
                        {
                            unit: 'W of heat',
                            h: 240,
                            dp: 0,
                            marks: 1,
                            series: [
                                S('Heat target', d.heattarget, C.violet),
                                S('Heat request', d.heatw, C.orange),
                                S('Integral', m.integral, C.orange, { dash: '5 4', w: 1.5 }),
                                S('Demand level', m.levH, C.ink, { step: 1, w: 1.5 }),
                                S('Heat output', d.heatout, C.aqua, { w: 1, alpha: 0.6 }),
                                S('Heat output, 2 h mean', m.hm, C.aqua),
                                S('Proportional', m.prop, C.muted, { w: 1.5, off: 1 })
                            ]
                        },
                        {
                            unit: 'Heat deficit, Wh',
                            h: 96,
                            dp: 0,
                            one: 1,
                            zero: 1,
                            wash: 1,
                            series: [S('Heat deficit', d.deficit, C.violet)]
                        }
                    ]
                },
                {
                    title: 'Compressor side',
                    sub: 'What the unit was told and what it drew.',
                    lane: 1,
                    strips: [
                        {
                            unit: 'W of electricity',
                            h: 150,
                            dp: 0,
                            zero: 1,
                            series: [
                                S('Power', d.power, C.red),
                                S('demand.table for the level', m.levW, C.ink, { step: 1, w: 1.5 })
                            ]
                        },
                        {
                            unit: 'Compressor, Hz',
                            h: 96,
                            dp: 0,
                            one: 1,
                            zero: 1,
                            series: [S('Compressor', d.comp, C.ink2)]
                        }
                    ]
                },
                {
                    title: 'Air side',
                    sub: 'The three readings the heat output estimate is made from.',
                    strips: [
                        {
                            unit: U,
                            h: 170,
                            dp: 1,
                            series: [S('Liquid', m.liquid, C.violet), S('Unit room', m.intake, C.orange)]
                        },
                        { unit: 'Fan, rpm', h: 96, dp: 0, one: 1, zero: 1, series: [S('Fan', d.fan, C.green)] }
                    ]
                },
                {
                    title: 'Efficiency',
                    sub: 'Heat output divided by power. The 6 h mean counts the defrosts in.',
                    strips: [
                        {
                            unit: 'COP',
                            h: 130,
                            dp: 2,
                            zero: 1,
                            series: [
                                S('COP', m.cop, C.blue, { w: 1, alpha: 0.5 }),
                                S('COP, 6 h mean', m.cm, C.blue),
                                S('Heat per watt of the demand table', m.scale, C.orange, { step: 1, w: 1.5, off: 1 })
                            ]
                        }
                    ]
                }
            ];
            function any(se) {
                return se.v.some(function (v) {
                    return v != null;
                });
            }
            return g.filter(function (gr) {
                // Only what the log has, the time axis under the last strip of each group
                gr.strips = gr.strips.filter(function (s) {
                    s.series = s.series.filter(any);
                    s.series.forEach(function (se) {
                        se.key = s.unit + '/' + se.name;
                        if (se.key in OFF) se.off = OFF[se.key];
                    });
                    return s.series.length;
                });
                if (gr.strips.length) gr.strips[gr.strips.length - 1].x = 1;
                return gr.strips.length;
            });
        }

        // ---- drawing ----
        function px(t) {
            return L + ((t - dom[0]) * pw) / (dom[1] - dom[0]);
        }
        function first(t, v) {
            // First sample at or after time v
            var lo = 0,
                hi = t.length;
            while (lo < hi) {
                var m = (lo + hi) >> 1;
                if (t[m] < v) lo = m + 1;
                else hi = m;
            }
            return lo;
        }
        function timeFmt(v, sec) {
            var o = { hour: '2-digit', minute: '2-digit' };
            if (sec) o.second = '2-digit';
            if (sec || dom[1] - dom[0] > 864e5) o.weekday = 'short';
            return new Date(v).toLocaleString([], o);
        }
        function num(v, dp) {
            return v == null
                ? '–'
                : v
                      .toLocaleString('en-US', { minimumFractionDigits: dp, maximumFractionDigits: dp })
                      .replace(/,/g, ' ');
        }
        function bands(g, top, h) {
            // Shading of the states
            var r = M.runs.filter(function (r) {
                    return r.b > dom[0] && r.a < dom[1];
                }),
                s = g.selectAll('rect').data(r);
            s.exit().remove();
            s.enter()
                .append('rect')
                .merge(s)
                .attr('x', function (r) {
                    return Math.max(L, px(r.a));
                })
                .attr('y', top)
                .attr('height', h)
                .attr('width', function (r) {
                    return Math.max(1, Math.min(L + pw, px(r.b)) - Math.max(L, px(r.a)));
                })
                .attr('fill', function (r) {
                    return SHADE[r.v][0];
                });
        }
        function makeStrip(parent, cfg) {
            var s = { cfg: cfg, T: 6, B: cfg.x ? 20 : 6 },
                hd = parent.append('div').attr('class', 'hd');
            hd.append('span').attr('class', 'un').text(cfg.unit);
            cfg.series.forEach(function (se) {
                var b = hd.append('button').attr('class', 'li').attr('type', 'button').attr('aria-pressed', !se.off);
                b.append('svg')
                    .attr('width', 22)
                    .attr('height', 10)
                    .append('line')
                    .attr('x1', 1)
                    .attr('x2', 21)
                    .attr('y1', 5)
                    .attr('y2', 5)
                    .attr('stroke', se.c)
                    .attr('stroke-width', se.w >= 2 ? 3 : 2)
                    .attr('stroke-dasharray', se.dash || null)
                    .attr('opacity', se.alpha || 1);
                if (!cfg.one) b.append('span').text(se.name);
                se.val = b.append('b');
                if (cfg.one) b.attr('disabled', true).attr('aria-pressed', null);
                else
                    b.on('click', function () {
                        OFF[se.key] = se.off = !se.off;
                        b.attr('aria-pressed', !se.off);
                        drawStrip(s);
                        hover();
                    });
            });
            s.svg = parent.append('svg').attr('class', 'plot').attr('font-family', FONT);
            s.clip = s.svg
                .append('clipPath')
                .attr('id', 'gc' + strips.length)
                .append('rect');
            s.gB = s.svg.append('g');
            s.gY = s.svg.append('g');
            s.gX = s.svg.append('g');
            s.gL = s.svg.append('g').attr('clip-path', 'url(#gc' + strips.length + ')');
            s.gM = s.svg.append('g');
            s.cross = s.svg.append('line').attr('stroke', C.muted).attr('display', 'none');
            s.gD = s.svg.append('g');
            strips.push(s);
            plots.push(s.svg.node());
        }
        function drawStrip(s) {
            var cfg = s.cfg,
                h = cfg.h,
                top = s.T,
                bot = h - s.B,
                lo = Infinity,
                hi = -Infinity;
            var vis = cfg.series.filter(function (se) {
                return !se.off;
            });
            vis.forEach(function (se) {
                // The scale fits what is in the range shown
                for (var i = I0; i <= I1; i++) {
                    var v = se.v[i];
                    if (v != null) {
                        if (v < lo) lo = v;
                        if (v > hi) hi = v;
                    }
                }
            });
            if (lo == Infinity) {
                lo = 0;
                hi = 1;
            }
            if (cfg.zero) {
                lo = Math.min(lo, 0);
                hi = Math.max(hi, 0);
            }
            if (lo == hi) {
                lo -= 1;
                hi += 1;
            }
            var pad = (hi - lo) * 0.04,
                n = Math.max(3, Math.round((bot - top) / 42));
            var y = (s.y = d3
                .scaleLinear()
                .domain([lo >= 0 && lo - pad < 0 ? 0 : lo - pad, hi + pad])
                .range([bot, top])
                .nice(n));
            s.svg.attr('width', W).attr('height', h);
            s.clip
                .attr('x', L)
                .attr('y', top)
                .attr('width', pw)
                .attr('height', bot - top);
            bands(s.gB, top, bot - top);
            var ty = s.gY.selectAll('g').data(y.ticks(n));
            ty.exit().remove();
            var te = ty.enter().append('g');
            te.append('line');
            te.append('text');
            ty = te.merge(ty).attr('transform', function (v) {
                return 'translate(0,' + (Math.round(y(v)) + 0.5) + ')';
            });
            ty.select('line')
                .attr('x1', L)
                .attr('x2', L + pw)
                .attr('stroke', function (v) {
                    return v == 0 && lo < 0 ? C.axis : C.grid;
                });
            ty.select('text')
                .attr('x', L - 6)
                .attr('dy', '.32em')
                .attr('text-anchor', 'end')
                .attr('font-size', 11)
                .attr('fill', C.muted)
                .text(function (v) {
                    return v.toLocaleString('en-US').replace(/,/g, ' ');
                });
            var xs = d3
                .scaleTime()
                .domain(dom)
                .range([L, L + pw]);
            var tx = s.gX.selectAll('g').data(xs.ticks(Math.max(2, Math.floor(pw / 95))));
            tx.exit().remove();
            te = tx.enter().append('g');
            te.append('line');
            te.append('text');
            tx = te.merge(tx).attr('transform', function (v) {
                return 'translate(' + (Math.round(xs(v)) + 0.5) + ',0)';
            });
            tx.select('line').attr('y1', top).attr('y2', bot).attr('stroke', C.grid);
            tx.select('text')
                .attr('y', bot + 14)
                .attr('text-anchor', 'middle')
                .attr('font-size', 11)
                .attr('fill', C.muted)
                .text(function (v) {
                    return cfg.x ? timeFmt(v) : '';
                });
            var ln = s.gL.selectAll('path.ln').data(vis, function (se) {
                return se.name;
            });
            ln.exit().remove();
            ln.enter()
                .append('path')
                .attr('class', 'ln')
                .attr('fill', 'none')
                .attr('stroke-linejoin', 'round')
                .merge(ln)
                .order()
                .attr('stroke', function (se) {
                    return se.c;
                })
                .attr('stroke-width', function (se) {
                    return se.w;
                })
                .attr('stroke-dasharray', function (se) {
                    return se.dash || null;
                })
                .attr('opacity', function (se) {
                    return se.alpha || 1;
                })
                .attr('d', function (se) {
                    return Graph.path(M.t, se.v, se.step, px, y, I0, I1, pw);
                });
            var wash = s.gL.selectAll('path.z').data(cfg.wash && vis.length ? [vis[0]] : []); // From zero to the line
            wash.exit().remove();
            wash.enter()
                .insert('path', ':first-child')
                .attr('class', 'z')
                .attr('stroke', 'none')
                .attr('opacity', 0.12)
                .merge(wash)
                .attr('fill', function (se) {
                    return se.c;
                })
                .attr('d', function (se) {
                    var area = d3
                        .area()
                        .defined(function (i) {
                            return se.v[i] != null;
                        })
                        .x(function (i) {
                            return px(M.t[i]);
                        })
                        .y0(y(0))
                        .y1(function (i) {
                            return y(se.v[i]);
                        });
                    return area(d3.range(I0, I1 + 1, Math.max(1, Math.ceil((I1 - I0 + 1) / pw))));
                });
            var mk = s.gM.selectAll('g').data(
                M.marks.filter(function (k) {
                    return k.t > dom[0] && k.t < dom[1];
                })
            );
            mk.exit().remove();
            te = mk.enter().append('g');
            te.append('line');
            te.append('text');
            mk = te.merge(mk).attr('transform', function (k) {
                return 'translate(' + px(k.t) + ',0)';
            });
            mk.select('line').attr('y1', top).attr('y2', bot).attr('stroke', C.ink2).attr('stroke-dasharray', '3 3');
            mk.select('text')
                .attr('x', 4)
                .attr('y', top + 11)
                .attr('font-size', 11)
                .attr('fill', C.ink2)
                .text(function (k) {
                    return cfg.marks ? k.s : '';
                });
        }
        function drawLane() {
            // The demand level as a band, darker for more watts in demand.table, named where the name fits
            if (!lane) return;
            var h = 24,
                r = M.levRuns.filter(function (r) {
                    return r.b > dom[0] && r.a < dom[1];
                });
            var ws = M.levels.map(function (k) {
                    return M.tab[k];
                }),
                w0 = d3.min(ws),
                w1 = d3.max(ws);
            lane.attr('width', W).attr('height', h);
            var g = lane.selectAll('g').data(r);
            g.exit().remove();
            var e = g.enter().append('g');
            e.append('rect');
            e.append('text');
            g = e.merge(g);
            function x0(r) {
                return Math.max(L, px(r.a));
            }
            function wd(r) {
                return Math.max(1, Math.min(L + pw, px(r.b)) - x0(r) - 1);
            }
            function ix(r) {
                return w1 > w0 ? Math.round(((M.tab[r.v] - w0) * (RAMP.length - 1)) / (w1 - w0)) : 6;
            }
            g.select('rect')
                .attr('x', x0)
                .attr('y', 3)
                .attr('height', h - 6)
                .attr('width', wd)
                .attr('fill', function (r) {
                    return RAMP[ix(r)];
                });
            g.select('text')
                .attr('x', function (r) {
                    return x0(r) + wd(r) / 2;
                })
                .attr('y', h / 2)
                .attr('dy', '.34em')
                .attr('text-anchor', 'middle')
                .attr('font-size', 11)
                .attr('fill', function (r) {
                    return ix(r) >= 6 ? '#fff' : C.ink;
                })
                .text(function (r) {
                    return wd(r) > r.v.length * 7 + 8 ? r.v : '';
                });
        }
        function drawAll() {
            I0 = Math.max(0, first(M.t, dom[0]) - 1);
            I1 = Math.min(M.k - 1, first(M.t, dom[1]));
            strips.forEach(drawStrip);
            drawLane();
            hover();
            zb.selectAll('button').attr('aria-pressed', function () {
                var s = +this.dataset.s * 1000;
                return s ? Math.abs(dom[1] - dom[0] - s) < 1000 && dom[1] == T1 : dom[0] == T0 && dom[1] == T1;
            });
            clearTimeout(anT);
            anT = setTimeout(summary, 200);
        }
        function hover() {
            // A line down every strip and the values at that sample in every legend. The latest shown while not
            // hovering.
            var i = H == null ? I1 : H,
                x = px(M.t[i]),
                d = M.d,
                on = H != null && x >= L - 0.5 && x <= L + pw + 0.5;
            strips.forEach(function (s) {
                s.cross
                    .attr('display', on ? null : 'none')
                    .attr('x1', x)
                    .attr('x2', x)
                    .attr('y1', s.T)
                    .attr('y2', s.cfg.h - s.B);
                var vis = s.cfg.series.filter(function (se) {
                    return !se.off && se.v[i] != null;
                });
                var c = s.gD.selectAll('circle').data(on ? vis : []);
                c.exit().remove();
                c.enter()
                    .append('circle')
                    .attr('r', 3.5)
                    .attr('stroke', '#fff')
                    .attr('stroke-width', 1.5)
                    .merge(c)
                    .attr('cx', x)
                    .attr('cy', function (se) {
                        return s.y(se.v[i]);
                    })
                    .attr('fill', function (se) {
                        return se.c;
                    });
                s.cfg.series.forEach(function (se) {
                    se.val.text(se.off ? '' : num(se.v[i], s.cfg.dp));
                });
            });
            var parts = [STATE[M.st(i)] || M.st(i)];
            if (SUB[M.sb(i)]) parts.push(SUB[M.sb(i)]);
            if (M.lev[i] != null) parts.push('demand ' + M.lev[i]);
            else if (d.demand[i] != null && M.st(i) != 'off')
                parts.push('demand ' + d.demand[i] + (d.quiet[i] ? ', quiet outdoor' : ''));
            if (d.fanset[i] != null) parts.push('fan ' + ['auto', 1, 2, 3, 4, 5, 'quiet'][d.fanset[i]]);
            if (M.tune(i)) parts.push(M.tune(i));
            bar.text('');
            bar.append('b').text((H == null ? 'Latest shown, ' : '') + timeFmt(M.t[i], 1));
            bar.append('span').text('  ·  ' + parts.join('  ·  '));
        }
        function schedule() {
            if (!raf)
                raf = requestAnimationFrame(function () {
                    raf = 0;
                    drawAll();
                });
        }
        function setDom(a, b, from) {
            // Show from a to b (ms), kept within the log and at least MINSPAN, on every strip and the overview
            if (b - a < MINSPAN) {
                var c = (a + b) / 2;
                a = c - MINSPAN / 2;
                b = c + MINSPAN / 2;
            }
            if (a < T0) {
                b += T0 - a;
                a = T0;
            }
            if (b > T1) {
                a -= b - T1;
                b = T1;
            }
            dom = [Math.max(T0, a), b];
            var k = (T1 - T0) / (dom[1] - dom[0]),
                t = d3.zoomIdentity.translate(L - k * xFull(dom[0]), 0).scale(k);
            plots.forEach(function (n) {
                n.__zoom = t;
            }); // d3.zoom keeps its state there
            if (from != 'brush') ovB.call(brush.move, [xFull(dom[0]), xFull(dom[1])]);
            schedule();
        }
        function layout() {
            // Sizes, the overview and the zoom, for the width there is
            W = Math.max(320, charts.node().clientWidth || 1000);
            pw = W - L - R;
            xFull = d3
                .scaleLinear()
                .domain([T0, T1])
                .range([L, L + pw]);
            var h = 56,
                keep = dom;
            ov.attr('width', W).attr('height', h).selectAll('*').remove();
            dom = [T0, T1];
            bands(ov.append('g'), 4, h - 22);
            var lo = d3.min(M.room),
                hi = d3.max(M.room);
            var y = d3
                .scaleLinear()
                .domain(lo == null || lo == hi ? [0, 1] : [lo, hi])
                .range([h - 21, 7]);
            ov.append('path')
                .attr('fill', 'none')
                .attr('stroke', C.red)
                .attr('stroke-width', 1.5)
                .attr('d', Graph.path(M.t, M.room, 0, px, y, 0, M.k - 1, pw));
            var xs = d3
                .scaleTime()
                .domain(dom)
                .range([L, L + pw]);
            ov.append('g')
                .selectAll('text')
                .data(xs.ticks(Math.max(2, Math.floor(pw / 95))))
                .enter()
                .append('text')
                .attr('x', function (v) {
                    return xs(v);
                })
                .attr('y', h - 5)
                .attr('text-anchor', 'middle')
                .attr('font-size', 11)
                .attr('fill', C.muted)
                .text(function (v) {
                    return timeFmt(v);
                });
            dom = keep;
            brush = d3
                .brushX()
                .extent([
                    [L, 2],
                    [L + pw, h - 18]
                ])
                .on('brush end', function (e) {
                    if (!e.sourceEvent) return;
                    if (e.selection) setDom(xFull.invert(e.selection[0]), xFull.invert(e.selection[1]), 'brush');
                    else if (e.type == 'end') setDom(T0, T1);
                });
            ovB = ov.append('g').call(brush);
            ovB.select('.selection').attr('fill', C.blue).attr('fill-opacity', 0.15).attr('stroke', C.blue);
            zoom = d3
                .zoom()
                .scaleExtent([1, Math.max(1, (T1 - T0) / MINSPAN)])
                .extent([
                    [L, 0],
                    [L + pw, 100]
                ])
                .translateExtent([
                    [L, 0],
                    [L + pw, 100]
                ]) // Only x is used
                .filter(function (e) {
                    return !e.button && (e.type != 'wheel' || e.ctrlKey || e.metaKey) && e.type != 'dblclick';
                })
                .on('zoom', function (e) {
                    if (!e.sourceEvent) return;
                    var node = this,
                        x = e.transform.rescaleX(xFull);
                    dom = [Math.max(T0, x.invert(L)), Math.min(T1, x.invert(L + pw))];
                    plots.forEach(function (n) {
                        if (n !== node) n.__zoom = e.transform;
                    });
                    ovB.call(brush.move, [xFull(dom[0]), xFull(dom[1])]);
                    schedule();
                });
            strips.forEach(function (s) {
                s.svg
                    .call(zoom)
                    .on('dblclick', function () {
                        setDom(T0, T1);
                    })
                    .on('pointermove pointerdown', function (e) {
                        var t = dom[0] + ((d3.pointer(e, this)[0] - L) * (dom[1] - dom[0])) / pw,
                            i = Math.min(M.k - 1, first(M.t, t));
                        if (i !== H) {
                            H = i;
                            hover();
                        }
                    })
                    .on('pointerleave', function (e) {
                        if (e.pointerType != 'mouse') return; // A tap stays until the next one
                        H = null;
                        hover();
                    });
            });
            setDom(dom[0], dom[1]);
        }

        // ---- the range shown, summed up ----
        var PW = 320,
            PH = 215,
            PL = 40,
            PR = 10,
            PT = 12,
            PB = 34;
        function panel(title, sub) {
            var p = an.append('div');
            p.append('h3').text(title);
            p.append('p').text(sub);
            return p
                .append('svg')
                .attr('viewBox', '0 0 ' + PW + ' ' + PH)
                .attr('font-family', FONT);
        }
        function axes(svg, x, y, xl, yl, xt) {
            var g = svg.append('g');
            y.ticks(5).forEach(function (v) {
                g.append('line')
                    .attr('x1', PL)
                    .attr('x2', PW - PR)
                    .attr('y1', y(v))
                    .attr('y2', y(v))
                    .attr('stroke', v == 0 ? C.axis : C.grid);
                g.append('text')
                    .attr('x', PL - 5)
                    .attr('y', y(v))
                    .attr('dy', '.32em')
                    .attr('text-anchor', 'end')
                    .attr('font-size', 11)
                    .attr('fill', C.muted)
                    .text(v);
            });
            xt.forEach(function (v) {
                g.append('text')
                    .attr('x', x(v))
                    .attr('y', PH - PB + 14)
                    .attr('text-anchor', 'middle')
                    .attr('font-size', 11)
                    .attr('fill', C.muted)
                    .text(v);
            });
            g.append('text')
                .attr('x', (PL + PW - PR) / 2)
                .attr('y', PH - 4)
                .attr('text-anchor', 'middle')
                .attr('font-size', 11)
                .attr('fill', C.ink2)
                .text(xl);
            g.append('text').attr('x', 2).attr('y', 9).attr('font-size', 11).attr('fill', C.ink2).text(yl);
        }
        function empty(svg, s) {
            svg.append('text')
                .attr('x', PW / 2)
                .attr('y', PH / 2)
                .attr('text-anchor', 'middle')
                .attr('font-size', 12)
                .attr('fill', C.muted)
                .text(s);
        }
        function dot(svg, x, y) {
            svg.append('circle')
                .attr('cx', x)
                .attr('cy', y)
                .attr('r', 4.5)
                .attr('fill', C.blue)
                .attr('stroke', '#fff')
                .attr('stroke-width', 2);
        }
        function summary() {
            an.selectAll('*').remove();
            var svg = panel(
                'Power at each demand level',
                'Dot: median power drawn. Bar: 10th to 90th percentile. Black line: demand.table. ' +
                    'Numbers: median / table.'
            );
            var lv = Graph.levelPower(M, I0, I1);
            if (!lv.length) empty(svg, 'No steady demand level in this range');
            else {
                var x = d3
                    .scaleBand()
                    .domain(
                        lv.map(function (r) {
                            return r.key;
                        })
                    )
                    .range([PL, PW - PR])
                    .padding(0.25);
                var y = d3
                        .scaleLinear()
                        .domain([
                            0,
                            d3.max(lv, function (r) {
                                return Math.max(r.hi, r.table);
                            }) * 1.15
                        ])
                        .nice(5)
                        .range([PH - PB, PT]),
                    tw = Math.min(46, x.bandwidth());
                axes(
                    svg,
                    function (k) {
                        return x(k) + x.bandwidth() / 2;
                    },
                    y,
                    'Demand level',
                    'W of electricity',
                    x.domain()
                );
                lv.forEach(function (r) {
                    var cx = x(r.key) + x.bandwidth() / 2;
                    svg.append('line')
                        .attr('x1', cx)
                        .attr('x2', cx)
                        .attr('y1', y(r.lo))
                        .attr('y2', y(r.hi))
                        .attr('stroke', RAMP[3])
                        .attr('stroke-width', 4)
                        .attr('stroke-linecap', 'round');
                    svg.append('line')
                        .attr('x1', cx - tw / 2)
                        .attr('x2', cx + tw / 2)
                        .attr('y1', y(r.table))
                        .attr('y2', y(r.table))
                        .attr('stroke', C.ink)
                        .attr('stroke-width', 2);
                    dot(svg, cx, y(r.median));
                    svg.append('text')
                        .attr('x', cx)
                        .attr('y', y(Math.max(r.hi, r.table)) - 7)
                        .attr('text-anchor', 'middle')
                        .attr('font-size', 11)
                        .attr('fill', C.ink)
                        .text(Math.round(r.median) + ' / ' + r.table);
                });
            }
            var starts = Graph.defrostStarts(M, I0, I1),
                pts = Graph.cycles(M, starts);
            svg = panel(
                'COP by outdoor temperature',
                'Each dot is one defrost cycle, from one defrost to the next, the defrost counted in.'
            );
            if (pts.length < 2) empty(svg, 'Needs at least three defrosts in the range');
            else {
                var ex = d3.extent(pts, function (p) {
                        return p.outdoor;
                    }),
                    ey = d3.extent(pts, function (p) {
                        return p.cop;
                    });
                var x2 = d3
                    .scaleLinear()
                    .domain([ex[0] - 0.5, ex[1] + 0.5])
                    .nice(5)
                    .range([PL, PW - PR]);
                var y2 = d3
                    .scaleLinear()
                    .domain([Math.max(0, ey[0] - 0.3), ey[1] + 0.3])
                    .nice(5)
                    .range([PH - PB, PT]);
                axes(svg, x2, y2, 'Outdoor, ' + U, 'COP', x2.ticks(5));
                pts.forEach(function (p) {
                    dot(svg, x2(p.outdoor), y2(p.cop));
                });
            }
            svg = panel(
                'Room temperature through a defrost',
                'Each thin line is one defrost, the heavy line the median. From 10 minutes before to 45 after.'
            );
            var th = Graph.through(M, starts, 10, 45);
            if (!th.lines.length) empty(svg, 'No defrost in this range');
            else {
                var lo = d3.min(th.lines, function (l) {
                        return d3.min(l);
                    }),
                    hi = d3.max(th.lines, function (l) {
                        return d3.max(l);
                    });
                var x3 = d3
                    .scaleLinear()
                    .domain([-10, 45])
                    .range([PL, PW - PR]);
                var y3 = d3
                    .scaleLinear()
                    .domain([lo - 0.05, hi + 0.05])
                    .nice(5)
                    .range([PH - PB, PT]);
                var gen = d3
                    .line()
                    .defined(function (v) {
                        return v != null;
                    })
                    .x(function (v, j) {
                        return x3((th.a0 + j) / th.per);
                    })
                    .y(function (v) {
                        return y3(v);
                    });
                axes(
                    svg,
                    x3,
                    y3,
                    'Minutes from the start of the defrost',
                    U + ' from where it stood',
                    [-10, 0, 10, 20, 30, 40]
                );
                svg.append('line')
                    .attr('x1', x3(0))
                    .attr('x2', x3(0))
                    .attr('y1', PT)
                    .attr('y2', PH - PB)
                    .attr('stroke', C.axis);
                th.lines.forEach(function (l) {
                    svg.append('path')
                        .attr('d', gen(l))
                        .attr('fill', 'none')
                        .attr('stroke', C.blue)
                        .attr('opacity', 0.3);
                });
                svg.append('path')
                    .attr('d', gen(th.median))
                    .attr('fill', 'none')
                    .attr('stroke', C.blue)
                    .attr('stroke-width', 2.5);
                if (th.low && th.low.v < 0) {
                    dot(svg, x3(th.low.min), y3(th.low.v));
                    svg.append('text')
                        .attr('x', x3(th.low.min) + 8)
                        .attr('y', y3(th.low.v) + 13)
                        .attr('font-size', 11)
                        .attr('fill', C.ink)
                        .text(
                            th.low.v.toFixed(2).replace('-', '−') +
                                ' ' +
                                U +
                                ' after ' +
                                Math.round(th.low.min) +
                                ' min'
                        );
                }
            }
        }

        // ---- the page ----
        function show(d, at) {
            // Draw the log d, read at time at (ms). After a refresh the range shown stays, following the latest sample
            // if it did.
            if (!d.ago.length) {
                msg.text('No samples yet, the first comes after 15 s.');
                return;
            }
            var was = M && { dom: dom, all: dom[0] == T0 && dom[1] == T1, latest: dom[1] == T1 };
            M = Graph.prep(d, at, F);
            T0 = M.t[0] - M.iv;
            T1 = M.t[M.k - 1];
            if (!was || was.all || saved) dom = SP && !was && SP * 1000 < T1 - T0 ? [T1 - SP * 1000, T1] : [T0, T1];
            else dom = was.latest ? [T1 - (was.dom[1] - was.dom[0]), T1] : was.dom;
            H = null;
            strips = [];
            plots = [];
            lane = null;
            msg.text(
                (saved ? 'Saved log, last ' : 'Last ') +
                    (d.ago[0] >= 172800
                        ? Math.round(d.ago[0] / 8640) / 10 + ' days'
                        : Math.round(d.ago[0] / 360) / 10 + ' hours') +
                    ', to ' +
                    new Date(T1).toLocaleString()
            );
            charts.selectAll('*').remove();
            groupsFor(M).forEach(function (g) {
                var sec = charts.append('section').attr('class', 'g'),
                    gh = sec.append('div').attr('class', 'gh');
                gh.append('h2').text(g.title);
                gh.append('span').text(g.sub);
                if (g.lane && M.levRuns.length) {
                    var lh = sec.append('div').attr('class', 'hd');
                    lh.append('span').attr('class', 'un').text('Demand level');
                    lh.append('span').attr('class', 'hint').text('darker for more watts in demand.table');
                    lane = sec.append('svg').attr('font-family', FONT);
                }
                g.strips.forEach(function (s) {
                    makeStrip(sec, s);
                });
            });
            zb.attr('hidden', null)
                .selectAll('button')
                .each(function () {
                    var s = +this.dataset.s;
                    this.hidden = s > 0 && s * 1000 >= T1 - T0;
                    this.onclick = function () {
                        SP = s;
                        address();
                        setDom(s ? T1 - s * 1000 : T0, T1);
                    };
                });
            layout();
        }
        function load() {
            fetch('/graph.json')
                .then(function (r) {
                    return r.json();
                })
                .then(function (d) {
                    if (!saved) show(d, Date.now());
                })
                .catch(function (e) {
                    if (!M) msg.text('Could not load the log: ' + e);
                })
                .finally(function () {
                    clearTimeout(rfT);
                    if (RF && !saved) rfT = setTimeout(load, RF * 1000);
                });
        }
        function resized() {
            clearTimeout(rzT);
            rzT = setTimeout(function () {
                if (M && Math.max(320, charts.node().clientWidth || 1000) != W) layout();
            }, 150);
        }
        if (window.ResizeObserver) new ResizeObserver(resized).observe(charts.node());
        else window.addEventListener('resize', resized);
        load();
    })();
