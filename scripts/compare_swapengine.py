#!/usr/bin/env python3
"""The MX head-to-head comparison driver (ROADMAP.md section MX; DECISIONS.md D21, D71).

Feeds the neutral exchange file written by `tools/compare/compare_ois` to a SECOND engine through
that engine's own public JSON interface, and diffs its answers against this engine's.

D11 / D21: the second engine is a BLACK BOX. This script executes one binary of it and reads that
binary's stdout. It opens no source, header, fixture or build script of it, and nothing derived
from it is written back into this repository. The only thing that crosses the boundary is the
exchange file, which THIS repository wrote from its own blueprints, and the numbers that come
back, which go into a report.

The request schema below was established by black-box probing of the CLI's own error messages
(D71 records the probe); it is a description of an external interface this repository targets,
not code taken from anywhere.

Usage:
  scripts/compare_swapengine.py --exchange <exchange.json> --results <epykos.json> \
      --cli <path to the other engine's JSON CLI> [--json <out.json>] [--per-trade]

Exit status: 0 when every compared quantity is inside its tolerance, 1 when one is not, 2 on a
usage or transport error. A non-zero exit means the two engines are NOT computing the same thing
and no timing comparison based on this fixture is meaningful.
"""
import argparse
import json
import subprocess
import sys

# Agreement tolerances. Relative unless the quantity can legitimately be ~0, in which case the
# comparison is relative to the row's own scale (see `rel`).
TOL = {
    'discount': 1e-12,       # the calibrated curve itself
    'par_rate': 1e-11,       # each calibration instrument's model quote
    'npv': 1e-10,            # book and per-trade present value, relative to the book's gross scale
    'ladder': 1e-9,          # d(PV)/d(quote), relative to the ladder's own infinity norm
}


def die(msg, code=2):
    print('compare_swapengine: ' + msg, file=sys.stderr)
    sys.exit(code)


def run_cli(cli, request):
    try:
        p = subprocess.run([cli], input=json.dumps(request), capture_output=True, text=True, timeout=600)
    except FileNotFoundError:
        die('cannot execute ' + cli)
    except subprocess.TimeoutExpired:
        die('the other engine did not answer within 600 s')
    out = p.stdout or p.stderr
    try:
        j = json.loads(out)
    except Exception:
        die('the other engine did not return JSON: ' + out[:400])
    if 'error' in j:
        die('the other engine rejected the request: ' + str(j['error'])[:400])
    return j


def obs_of(c, daily):
    """One compounded coupon's observation, as sub-periods.

    `daily=False` collapses the observation period to ONE sub-period [t_start, t_end). That is
    the algebraically identical, telescoped form: for the plain observation method the daily
    overnight forwards are DF ratios whose product is exactly DF(t_start)/DF(t_end), so the two
    forms agree to rounding and differ by two orders of magnitude in arithmetic. It is the
    "identity sub-periods" fast path of `docs/PRIOR_ART.md` section 1, applied at the input.

    `daily=True` hands over every projected observation day, which is the decomposition
    EpykosEngine itself evaluates. It was ADDED to make the two engines do the same work and it
    DOES NOT: measured (D71), splitting one observation period into equal sub-periods changes the
    other engine's calibrated curve --- one sub-period over [0, 1] gives DF(1) = 0.961538461538461
    and rate exactly par, two equal sub-periods give 0.961168781237985 and four give
    0.960980344482816 --- so its `obs` sub-period list is not a telescoping product of discount
    factor ratios and cannot carry a daily ACT/360 decomposition with the semantics this engine's
    coupon has. Establishing what it IS would mean reading that engine's source, which D11
    forbids. So `--daily` is kept only as the evidence for that finding: it reports a
    DISAGREEMENT and exits 1, and no timing is quoted from it.
    """
    if not daily:
        return {'sub_start': [c['t_start']], 'sub_end': [c['t_end']], 'tau_index': c['tau_obs']}
    days = c.get('obs_days')
    if days is None:
        die('--daily needs an exchange file written with --obs-days')
    return {'sub_start': [d[0] for d in days], 'sub_end': [d[1] for d in days],
            'tau_index': c['tau_obs']}


def bundle_from_exchange(x, daily=False):
    """The exchange file's curve and calibration instruments in the other engine's bundle shape.

    Every knot goes in the `meeting` region and `back` is left empty, which is the setting under
    which that engine's bundle curve is piecewise-constant instantaneous forward -- the same
    family as this fixture's log-linear discount factors, so neither side's interpolation is
    assumed (D71).
    """
    insts = []
    for i in x['instruments']:
        fwd = [{'obs': obs_of(c, daily), 'pay': c['t_pay'], 'tau_pay': c['tau_accrual']}
               for c in i['float']]
        fix = [{'pay': c['t_pay'], 'tau': c['tau_accrual']} for c in i['fixed']]
        insts.append({'quote': 'ParRate',
                      'fwd': {'forecast': 0, 'discount': 0, 'coupons': fwd},
                      'fixed': {'discount': 0, 'coupons': fix},
                      'market': i['market']})
    return {'curves': [{'meeting': list(x['knots']), 'back': [], 'base': -1, 'currency': 0}],
            'instruments': insts}


def book_from_exchange(x, trades=None):
    """The exchange file's book as the other engine's typed trades.

    side +1 receives fixed, so it PAYS float.
    """
    rows = x['book'] if trades is None else trades
    out = []
    for t in rows:
        out.append({'index': x['index'], 'type': 'ois', 'notional': t['notional'],
                    'discount_index': x['index'],
                    'pay': 'float' if t['side'] > 0 else 'fixed',
                    'fixed_rate': t['fixed_rate'],
                    'effective': t['effective'], 'maturity': t['termination']})
    return {'value_date': x['valuation'], 'curve_roles': {x['index']: 0}, 'trades': out}


def rel(a, b, scale):
    return abs(a - b) / scale if scale > 0 else abs(a - b)


def worst(name, ours, theirs, scale, tol, rows):
    if len(ours) != len(theirs):
        die('%s: %d values from us, %d from the other engine' % (name, len(ours), len(theirs)))
    w, at = 0.0, -1
    for i, (a, b) in enumerate(zip(ours, theirs)):
        e = rel(a, b, scale)
        if e > w:
            w, at = e, i
    rows.append({'quantity': name, 'n': len(ours), 'worst_relative': w, 'at': at,
                 'tolerance': tol, 'ok': w <= tol})
    return w <= tol


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exchange', required=True)
    ap.add_argument('--results', required=True)
    ap.add_argument('--cli', required=True)
    ap.add_argument('--json', default=None, help='write the full comparison record here')
    ap.add_argument('--per-trade', action='store_true',
                    help='also price every trade on its own, one request each (slow, exact)')
    ap.add_argument('--risk-samples', type=int, default=0, metavar='N',
                    help="after the agreement check passes, call the other engine N more times "
                         "on the SAME request and report min / median / p90 of the risk_us it "
                         "reports for itself. Each call is a fresh process, so every sample is "
                         "cold; that is stated alongside the number, never averaged away. Runs "
                         "only if the agreement check passed -- a disagreement means no timing.")
    ap.add_argument('--daily', action='store_true',
                    help="give the other engine every observation day too, instead of the "
                         "collapsed one-sub-period form: the setting in which both engines "
                         "evaluate the same arithmetic (needs --obs-days on the exchange file)")
    a = ap.parse_args()

    x = json.load(open(a.exchange))
    ours = json.load(open(a.results))
    if x.get('format') != 'epykos-compare-ois 1':
        die('unexpected exchange format ' + repr(x.get('format')))

    bundle = bundle_from_exchange(x, a.daily)
    book = book_from_exchange(x)
    req = {'bundle': bundle, 'sample_times': ours['sample_t'],
           'risk': True, 'portfolio_risk': book}
    j = run_cli(a.cli, req)

    pr = j.get('portfolio_risk')
    if pr is None:
        die('the other engine returned no portfolio_risk block')
    if pr['n'] != len(x['book']):
        die('the other engine priced %d of our %d trades' % (pr['n'], len(x['book'])))

    rows, ok = [], True
    # 1. the calibrated curve: discount factors at the sample times
    theirs_df = j['curves'][0]['discount']
    ok &= worst('discount_factor', ours['sample_df'], theirs_df, 1.0, TOL['discount'], rows)
    # 2. each calibration instrument's model par rate
    theirs_par = [d['model'] for d in j['quote_diagnostics']]
    ok &= worst('model_par_rate', ours['model_par_rate'], theirs_par, 1.0, TOL['par_rate'], rows)
    # 3. book NPV, relative to the book's gross scale (sum |trade PV|)
    gross = sum(abs(v) for v in ours['trade_npv']) or 1.0
    ok &= worst('book_npv', [ours['book_npv']], [pr['npv']], gross, TOL['npv'], rows)
    # 4. the ladder d(PV)/d(quote), relative to its own infinity norm
    lad = max(abs(v) for v in ours['book_ladder']) or 1.0
    ok &= worst('book_ladder', ours['book_ladder'], pr['ladder'], lad, TOL['ladder'], rows)

    per_trade = None
    if a.per_trade:
        theirs_pv = []
        for t in x['book']:
            one = book_from_exchange(x, [t])
            jt = run_cli(a.cli, {'bundle': bundle, 'portfolio_risk': one})
            theirs_pv.append(jt['portfolio_risk']['npv'])
        scale = max(abs(v) for v in ours['trade_npv']) or 1.0
        ok &= worst('trade_npv', ours['trade_npv'], theirs_pv, scale, TOL['npv'], rows)
        per_trade = theirs_pv

    print('%-18s %6s %16s %12s  %s' % ('quantity', 'n', 'worst relative', 'tolerance', 'verdict'))
    for r in rows:
        print('%-18s %6d %16.3e %12.0e  %s'
              % (r['quantity'], r['n'], r['worst_relative'], r['tolerance'],
                 'ok' if r['ok'] else 'FAIL (worst at index %d)' % r['at']))
    print()
    print('other engine: calibration converged=%s, iterations=%s, rank_deficiency=%s'
          % (j['calibration']['converged'], j['calibration']['iterations'],
             j['calibration'].get('rank_deficiency')))
    print('this engine : solve converged=%s, |Jtr|inf=%.3e'
          % (ours['solve']['converged'], ours['solve']['jtr_inf']))
    print('observation form given to the other engine: %s'
          % ('every observation day (same arithmetic as ours)' if a.daily
             else 'one sub-period per accrual (telescoped)'))
    print('self-reported risk time of the other engine: %.1f us for %d trades'
          % (pr['risk_us'], pr['n']))

    samples = None
    if a.risk_samples > 0:
        if not ok:
            die('--risk-samples refused: the agreement check failed, so no timing from this '
                'fixture means anything', 1)
        if a.daily:
            die('--risk-samples refused under --daily: that mode is a known disagreement, kept '
                'only as evidence (see obs_of)', 1)
        vals = []
        for _ in range(a.risk_samples):
            jt = run_cli(a.cli, req)
            vals.append(jt['portfolio_risk']['risk_us'])
        vals.sort()
        n = len(vals)
        samples = {'n': n, 'min': vals[0], 'median': vals[n // 2],
                   'p90': vals[min(n - 1, int(round(0.9 * n)) - 1)], 'max': vals[-1],
                   'raw': vals}
        print()
        print('other engine risk_us over %d COLD samples (one fresh process each), %d trades:'
              % (n, pr['n']))
        print('  min %.1f  median %.1f  p90 %.1f  max %.1f us'
              % (samples['min'], samples['median'], samples['p90'], samples['max']))
        print('  NOTE: this is its own self-reported risk time on an already-calibrated curve. '
              'Read it against bench/compare/README.md section 4 items 1, 3 and 4 before '
              'forming any ratio.')

    if a.json:
        json.dump({'rows': rows, 'ok': ok, 'daily': a.daily, 'risk_us_samples': samples,
                   'their_calibration': j['calibration'],
                   'their_risk_us': pr['risk_us'],
                   'their_ladder': pr['ladder'], 'their_curve_grad': pr['curve_grad'],
                   'their_npv': pr['npv'], 'their_par_rate': theirs_par,
                   'their_trade_npv': per_trade,
                   'our_ladder': ours['book_ladder'], 'our_npv': ours['book_npv']},
                  open(a.json, 'w'), indent=1)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
