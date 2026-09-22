# bench/results

Committed benchmark results, one directory per machine+toolchain fingerprint (D9, D13):

```
bench/results/<fingerprint-id>/<benchmark>.json
```

`<fingerprint-id>` is the 12-hex id printed by `scripts/fingerprint.sh`; store its JSON line alongside the results
(or inside each result file). Every result states the 1-minute load average and the flags. Numbers are only ever
compared within one fingerprint directory. Scratch output goes in `bench/results/<fingerprint-id>/tmp/`, which is
gitignored.
