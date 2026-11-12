// Host test for the data part of the heating graph page (graph.js): what is drawn from a /graph.json log
// Run with: make -C ESP test (needs node)
'use strict';
const G = require('../graph.js');

let failed = 0,
    passed = 0;
function check(name, cond) {
    if (cond) passed++;
    else {
        console.log('FAIL ' + name);
        failed++;
    }
}
function near(a, b, e) {
    return a != null && Math.abs(a - b) <= (e || 1e-9);
}

const TABLE = '40:270,95:640,100q:1510,100:2470';
const COLS = [
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
const AT = 1e12; // When the log was read (ms)
// A log of n samples, 15 s apart, the last one read as it was kept. f(i) gives the values of sample i, the rest are
// null.
function log(n, f, table) {
    const d = {
        interval: 15,
        table: table === undefined ? TABLE : table,
        ago: [],
        state: [],
        sub: [],
        states: ['off', 'idle', 'demand', 'cold', 'nosensor'],
        subs: ['', 'defrost', 'hold', 'rest']
    };
    COLS.forEach(c => {
        d[c] = [];
    });
    for (let i = 0; i < n; i++) {
        const v = Object.assign({ state: 'demand', sub: '' }, f(i));
        d.ago.push((n - 1 - i) * 15);
        d.state.push(d.states.indexOf(v.state));
        d.sub.push(d.subs.indexOf(v.sub));
        COLS.forEach(c => d[c].push(v[c] === undefined ? null : v[c]));
    }
    return d;
}

// Times, temperatures and the one room line
{
    const m = G.prep(
        log(3, i => ({ room: 2100 + i, smoothed: 2200 + i, outdoor: -550, liquid: 4000 })),
        AT,
        0
    );
    check('time of the last sample is when the log was read', m.t[2] == AT && m.t[0] == AT - 30000);
    check('room is the smoothed reading', near(m.room[1], 22.01));
    check('temperatures in degrees C', near(m.outdoor[0], -5.5) && near(m.liquid[0], 40));
    const f = G.prep(
        log(1, () => ({ smoothed: 2000, outdoor: -1000 })),
        AT,
        1
    );
    check('temperatures in degrees F', near(f.room[0], 68) && near(f.outdoor[0], 14));
    const r = G.prep(
        log(2, () => ({ room: 2150 })),
        AT,
        0
    );
    check('room is the raw reading in a log with no smoothed one', near(r.room[0], 21.5));
}

// The demand level, as the demand.table entry in use
{
    const m = G.prep(
        log(
            6,
            i =>
                [
                    { demand: 95, quiet: 1, scale: 300 },
                    { demand: 100, quiet: 1, scale: 300 },
                    { demand: 100, quiet: 0, scale: 250 },
                    { demand: 40, quiet: 1, state: 'idle', scale: 300 },
                    { demand: 100, quiet: 0, state: 'cold' },
                    { demand: 60, quiet: 1, scale: 300 }
                ][i]
        ),
        AT,
        0
    );
    check('level names', m.lev[0] == '95' && m.lev[1] == '100q' && m.lev[2] == '100');
    check('level watts from the table', m.levW[0] == 640 && m.levW[1] == 1510 && m.levW[2] == 2470);
    check(
        'level heat is the table watts times heat per watt',
        m.levH[0] == 1920 && m.levH[1] == 4530 && m.levH[2] == 6175
    );
    check('heating off is no watts and no level', m.lev[3] == null && m.levW[3] === 0 && m.levH[3] === 0);
    check('full power has no level', m.lev[4] == null && m.levW[4] == null);
    check('a demand not in the table has no level', m.lev[5] == null && m.levW[5] == null);
    check('levels in table order', m.levels.join() == '40,95,100q,100');
    const n = G.prep(
        log(1, () => ({ demand: 95, quiet: 1 }), null),
        AT,
        0
    );
    check('no table, no level', n.lev[0] == null && n.levels.length == 0);
}

// The PI parts: watts in demand control, a setpoint at full power
{
    const m = G.prep(
        log(2, i => (i ? { state: 'cold', heatw: null, integral: 2350 } : { heatw: 1500, integral: 1200 })),
        AT,
        0
    );
    check('proportional is the heat request less the integral', m.prop[0] == 300 && m.integral[0] == 1200);
    check('no watts integral at full power', m.integral[1] == null && m.prop[1] == null);
    check('setpoint integral at full power only', m.setint[0] == null && near(m.setint[1], 23.5));
}

// Means: heat output over 2 hours, COP over 6 hours with the defrosts counted in
{
    const m = G.prep(
        log(400, i =>
            i >= 200 && i < 240
                ? { sub: 'defrost', heatout: 0, power: 900 }
                : i == 250
                  ? { heatout: 500, power: 50 }
                  : { heatout: 3000, power: 1000 }
        ),
        AT,
        0
    );
    check('no mean heat output before half an hour of samples', m.hm[118] == null && near(m.hm[119], 3000));
    check('mean heat output counts the defrost in', m.hm[239] < 3000 && near(m.hm[239], (3000 * 200) / 240));
    check('COP of a sample', near(m.cop[0], 3));
    check('no COP in a defrost or at next to no power', m.cop[210] == null && m.cop[250] == null);
    check('no mean COP before half an hour', m.cm[118] == null && near(m.cm[119], 3));
    check('mean COP is all the heat over all the power', near(m.cm[239], (3000 * 200) / (1000 * 200 + 900 * 40)));
}

// Shading and the marks at a change of kp or ti
{
    const m = G.prep(
        log(8, i => ({
            sub: i == 2 || i == 3 ? 'defrost' : i == 4 ? 'hold' : '',
            state: i == 6 ? 'idle' : 'demand',
            kp: i < 5 ? 900 : 600,
            ti: 60
        })),
        AT,
        0
    );
    check('shaded runs', m.runs.map(r => r.v).join() == 'defrost,hold,idle');
    check('a run covers its samples', m.runs[0].a == m.t[1] && m.runs[0].b == m.t[3]);
    check(
        'one mark, where kp changes',
        m.marks.length == 1 && m.marks[0].t == m.t[4] && m.marks[0].s == 'kp 600 W/℃, ti 60 min'
    );
}

// Power drawn at each level: steady running only
{
    const m = G.prep(
        log(120, i =>
            i < 60
                ? {
                      demand: 95,
                      quiet: 1,
                      power: i < 8 ? 100 : 600 + (i % 3) * 10,
                      sub: i >= 40 && i < 50 ? 'defrost' : ''
                  }
                : { demand: 100, quiet: 1, power: i < 68 ? 700 : 1400 }
        ),
        AT,
        0
    );
    const p = G.levelPower(m, 0, 119);
    check('a row for each level seen, in table order', p.map(r => r.key).join() == '95,100q');
    check('table watts beside it', p[0].table == 640 && p[1].table == 1510);
    check('median leaves out the two minutes after a change', p[0].median == 610 && p[1].median == 1400);
    check('and the defrost, and the two minutes after it', p[0].n == 60 - 8 - 10 - 8);
    check('spread', p[0].lo == 600 && p[0].hi == 620);
    check('only the range asked for', G.levelPower(m, 0, 59).length == 1);
}

// Defrosts: the COP of each cycle, and the room through each
{
    const m = G.prep(
        log(700, i => {
            const c = i % 280,
                def = i >= 100 && c >= 100 && c < 140;
            return {
                sub: def ? 'defrost' : '',
                heatout: def ? 0 : 3000,
                power: def ? 500 : 1000,
                outdoor: i < 380 ? -500 : -700,
                smoothed: 2200 - (def ? (c - 100) * 2 : c >= 140 && c < 180 ? (180 - c) * 2 : 0)
            };
        }),
        AT,
        0
    );
    const s = G.defrostStarts(m, 0, 699);
    check('defrost starts', s.join() == '100,380,660');
    check('only in the range', G.defrostStarts(m, 200, 500).join() == '380');
    const c = G.cycles(m, s);
    check('one cycle between each two defrosts', c.length == 2);
    check('cycle COP counts its defrost in', near(c[0].cop, (3000 * 240) / (1000 * 240 + 500 * 40)));
    check('cycle outdoor temperature', near(c[0].outdoor, -5) && near(c[1].outdoor, -7));
    const th = G.through(m, s, 10, 45);
    check(
        'a line for each defrost, from 10 minutes before to 45 after',
        th.lines.length == 3 && th.lines[0].length == 55 * 4 + 1
    );
    check('from where the room stood when the defrost began', th.lines[0][40] === 0 && near(th.lines[0][80], -0.8));
    check('nothing past the end of the log', th.lines[2][80] == null && near(th.lines[2][79], -0.78));
    check(
        'median and its lowest point',
        near(th.median[80], -0.8) && th.low.j == 80 && near(th.low.min, 10) && near(th.low.v, -0.8)
    );
}

// The line through the samples: every sample when they are few, the least and most of each pixel column when many
{
    const t = [],
        v = [],
        x = ms => ms / 1000,
        y = w => 100 - w;
    for (let i = 0; i < 6; i++) {
        t.push(i * 1000);
        v.push(i == 3 ? null : i * 10);
    }
    check('line', G.path(t, v, 0, x, y, 0, 5, 100) == 'M0.0,100.0L1.0,90.0L2.0,80.0M4.0,60.0L5.0,50.0');
    check(
        'stepped line holds each value back to the sample before',
        G.path(t, v, 1, x, y, 0, 2, 100) == 'M0.0,100.0V90.0H1.0V80.0H2.0'
    );
    check('part of it', G.path(t, v, 0, x, y, 1, 2, 100) == 'M1.0,90.0L2.0,80.0');
    const T = [],
        V = [];
    for (let i = 0; i < 10000; i++) {
        T.push(i * 10);
        V.push(i == 5000 ? 90 : i == 7000 ? null : 20);
    }
    const p = G.path(T, V, 0, x, y, 0, 9999, 100);
    check('many samples: a short path', p.length < 100 * 40);
    check('many samples: a spike of one sample is kept', p.indexOf('V10.0') > 0);
    check('many samples: a gap is kept', p.split('M').length == 3);
}

console.log(passed + ' passed, ' + failed + ' failed');
process.exit(failed ? 1 : 0);
