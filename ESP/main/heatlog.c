// Saadin heating log (heatlog), see heatlog.h
#include <math.h>
#include <string.h>
#include "heatctl.h"
#include "heatlog.h"

static int16_t scaled(float v, float scale)
{ // v * scale, rounded and clamped, HEATLOG_NONE if not known
    if (isnan(v))
        return HEATLOG_NONE;
    float r = roundf(v * scale);
    if (r > INT16_MAX)
        return INT16_MAX;
    if (r <= HEATLOG_NONE)
        return HEATLOG_NONE + 1;
    return (int16_t)r;
}

static void reset(heatlog_t *l, uint32_t now)
{ // Start building a sample
    l->start = now;
    memset(l->sum, 0, sizeof(l->sum));
    memset(l->num, 0, sizeof(l->num));
    l->defrost = 0;
}

static void keep(heatlog_t *l, uint32_t now)
{ // Keep the sample built so far
    float mean[HEATLOG_MEANS];
    for (int i = 0; i < HEATLOG_MEANS; i++)
        mean[i] = l->num[i] ? l->sum[i] / l->num[i] : NAN;
    float pi = l->last.state == HEATCTL_COLD ? 100 : 1; // The COLD loop works on the setpoint, so finer units
    heatlog_sample_t s = {
        .t = now,
        .room = scaled(mean[0], 100),
        .outdoor = scaled(mean[1], 100),
        .heatw = scaled(mean[2], 1),
        .power = scaled(mean[3], 1),
        .smoothed = scaled(mean[4], 100),
        .liquid = scaled(mean[5], 100),
        .comp = scaled(mean[6], 1),
        .fan = scaled(mean[7], 1),
        .intake = scaled(mean[8], 100),
        .heatout = scaled(mean[9], 1),
        .target = scaled(l->last.target, 100),
        .effective = scaled(l->last.effective, 100),
        .setpoint = scaled(l->last.setpoint, 100),
        .demand = scaled(l->last.demand, 1),
        .integral = scaled(l->last.integral, pi),
        .owed = scaled(l->last.owed, 1),
        .give = scaled(mean[10], 1),
        .scale = scaled(l->last.scale, 100),
        .kp = scaled(l->last.kp, pi),
        .ti = scaled(l->last.ti, 1),
        .state = l->last.state,
        .sub = l->defrost ? HEATCTL_SUB_DEFROST : l->last.sub,
        .fanset = l->last.fanset,
        .quiet = l->last.quiet,
    };
    l->sample[l->next] = s;
    l->next = (l->next + 1) % l->n;
    if (l->count < l->n)
        l->count++;
    l->total++;
}

void heatlog_init(heatlog_t *l, heatlog_sample_t *buf, uint32_t n)
{
    memset(l, 0, sizeof(*l));
    l->sample = buf;
    l->n = buf ? n : 0;
}

void heatlog_add(heatlog_t *l, const heatlog_in_t *in, uint32_t now)
{
    if (!l->n)
        return;
    if (!l->start)
        reset(l, now);
    else if (now - l->start >= HEATLOG_INTERVAL) {
        keep(l, now);
        reset(l, now);
    }
    const float v[HEATLOG_MEANS] = {in->room, in->outdoor, in->heatw,  in->power,   in->smoothed, in->liquid,
                                    in->comp, in->fan,     in->intake, in->heatout, in->give};
    for (int i = 0; i < HEATLOG_MEANS; i++)
        if (!isnan(v[i])) {
            l->sum[i] += v[i];
            l->num[i]++;
        }
    if (in->sub == HEATCTL_SUB_DEFROST)
        l->defrost = 1;
    l->last = *in;
}

uint32_t heatlog_begin(const heatlog_t *l)
{
    return l->total - l->count;
}

uint32_t heatlog_end(const heatlog_t *l)
{
    return l->total;
}

uint32_t heatlog_read(const heatlog_t *l, uint32_t seq, heatlog_sample_t *out, uint32_t max)
{
    if (seq < heatlog_begin(l) || seq >= l->total)
        return 0;
    uint32_t c = l->total - seq; // Samples from seq to the newest
    uint32_t first = (l->next + l->n - c) % l->n;
    if (c > max)
        c = max;
    for (uint32_t i = 0; i < c; i++)
        out[i] = l->sample[(first + i) % l->n];
    return c;
}

float heatlog_heatout(float flow, float out, float intake)
{ // Air: 1.2 kg/m3 x 1005 J/(kg K), flow per hour
    return flow * (out - intake) * 1.2f * 1005 / 3600;
}

uint32_t heatlog_get(const heatlog_t *l, heatlog_sample_t *out, uint32_t max)
{
    uint32_t c = l->count < max ? l->count : max;
    for (uint32_t i = 0; i < c; i++) // The newest c, oldest first
        out[i] = l->sample[(l->next + l->n - c + i) % l->n];
    return c;
}
