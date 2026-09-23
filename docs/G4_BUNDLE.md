# G4_BUNDLE — researched market conventions with sources

Status: **M3/G0 (2026-09-23): USD and EUR sections** (Stage A of `PROBLEM.md`); **M3/G2 (2026-09-23): section 6, the instrument mechanics and curve definitions built on them**. GBP and JPY are M5/C1. Every
value in `blueprints/conventions/*.json` carries the citation given here (the JSON `sources` objects quote the same
passages); the registry test `tests/conventions/registry_test.cpp` asserts the values below.

Method and limits. Research was time-boxed (45 minutes) and done with `WebFetch` / `WebSearch` only (no browser tool:
a previous attempt hung). Primary documents harvested by the previous attempt (ECB €STR methodology, ISDA guidance and
market practice notes, ARRC user's guide, CFTC MAT chart, Bloomberg SEF and TP ICAP templates, LCH eligibility, EMMI
statements, Bundesbank TARGET list, Bianchetti–Carlicchi) were read from the scratchpad; the SIFMA, NY Fed, ECB, Eurex,
Strata and Federal Reserve pages were fetched today. CME pages timed out twice: the SR3 / SR1 facts come from CME's own
education pages and rulebook chapters as summarised by the search engine and a broker mirror, and are marked as such.
Anything that could not be sourced is marked **UNVERIFIED** and is never presented as more than that. No LIBOR-era
convention is presented as current.

The SwapEngine conventions file was read **as data only** (D37): `SwapEngine/conventions/conventions.json` and its
schema, `meta.sources` = OpenGamma Strata codified conventions; CME SOFR futures specs; ISDA RFR Conventions product
table (Oct 2021); ARRC xccy recommendations; 31 CFR 356 App. B; `meta.notes` "Ambiguities resolved with the desk
2026-07". Imported on 2026-09-23. Section 3 is the cross-check; a disagreement is recorded, not resolved by fiat.

Citations are `[n]` into section 5.

---

## 1. USD

### USD.1 Calendars

Three calendars matter for SOFR products, and they differ on Good Friday [4][5][7]:

| calendar (registry name) | definition | source |
|---|---|---|
| `USD-SIFMA` | U.S. Government Securities Business Day: "any day except for a Saturday, Sunday or a day on which [SIFMA] recommends that the fixed income departments of its members be closed for the entire day" (2021 ISDA Definitions, quoted in [5]); an **early close is a business day** | [1][5] |
| `USD-SOFR` | the days SOFR is published **for**: every USGS business day "including days for which SIFMA recommends an early close", "not published on, or for, days for which SIFMA recommends a full closure" [4] — **except Good Friday**: for 2026 the NY Fed announced "there will be no publication of SOFR on Friday, April 3, 2026 ... SOFR for Thursday, April 2, 2026 will be published on Monday, April 6, 2026 ... no publication of SOFR for Friday, April 3, 2026" [5, §1] | [4][5] |
| `USD-FED` | Federal Reserve Bank holidays (New York Business Days): "For holidays falling on Saturday, Federal Reserve Banks and Branches will be open the preceding Friday. For holidays falling on Sunday, ... closed the following Monday." | [7] |
| `USD-SOFR-SWAP` | joint SIFMA + Fed: the SOFR swap spot date "will be the day that is two U.S. Government Securities Business Days and New York Business Days following the Trade Date (i.e. the second day that is both ...)" [6]; payments "use the New York Business Day calendar" [6] | [6][8] |

Rules (the JSON rule instances), with the observance evidence:

| holiday | SIFMA rule | Fed rule | evidence |
|---|---|---|---|
| New Year's Day | Jan 1; Sun→Mon; **Sat not observed** | Jan 1; Sun→Mon; Sat open Fri | SIFMA 2027/2028: only an "Early Close ... Friday, December 31, 2027" [1]; Fed rule [7] |
| MLK Day | 3rd Mon Jan | same | 2026-01-19, 2027-01-18 [1][7] |
| Presidents Day | 3rd Mon Feb | same | 2026-02-16, 2027-02-15 [1][7] |
| Good Friday | Easter−2, **full close unless it is the first Friday of the month** (employment report: early close 12:00 ET) | not a Fed holiday | 2026-04-03 "Early Close (12:00 p.m. Eastern Time)" [1]; "April 3, 2026 (Good Friday) will be a U.S. Government Securities Business Day as confirmed by SIFMA" [5]; 2027-03-26 full close [1]. The first-Friday rule matches SIFMA's 2010, 2015, 2021 and 2026 practice; it is a **simplification** of the BLS release calendar |
| Memorial Day | last Mon May | same | 2026-05-25, 2027-05-31 [1][7] |
| Juneteenth | Jun 19 from 2022; Sat→Fri, Sun→Mon | Jun 19; Sat open Fri | SIFMA "Friday, June 18, 2027" [1]; Fed "June 19* (Saturday)" 2027 [7] |
| Independence Day | Jul 4; Sat→Fri, Sun→Mon | Jul 4; Sat open Fri | SIFMA "Friday, July 3, 2026", "Monday, July 5, 2027" [1]; Fed 2026 Sat, 2027 Sun→Mon [7] |
| Labor Day | 1st Mon Sep | same | 2026-09-07, 2027-09-06 [1][7] |
| Columbus Day | 2nd Mon Oct | same | 2026-10-12, 2027-10-11 [1][7] |
| Veterans Day | Nov 11; Sun→Mon; **Sat not observed** | Nov 11; Sat open Fri | 2026-11-11, 2027-11-11 [1][7]; the Saturday case (2028) follows SwapEngine's rule and SIFMA's 2023 practice — **UNVERIFIED** by fetch |
| Thanksgiving | 4th Thu Nov | same | 2026-11-26, 2027-11-25 [1][7] |
| Christmas | Dec 25; Sat→Fri, Sun→Mon | Dec 25; Sat open Fri | SIFMA "Friday, December 24, 2027" [1]; Fed 2027 "December 25* (Saturday)" [7] |

Published lists the registry reproduces (test `Calendar.Sifma*`, `Calendar.Sofr*`, `Calendar.FederalReserve*`):

| year | SIFMA full closes [1] | SOFR non-publication [4][5] | Federal Reserve [7] |
|---|---|---|---|
| 2026 | Jan 1, Jan 19, Feb 16, May 25, Jun 19, Jul 3, Sep 7, Oct 12, Nov 11, Nov 26, Dec 25 (Good Friday Apr 3 = early close) | SIFMA list **plus Apr 3** | Jan 1, Jan 19, Feb 16, May 25, Jun 19, Sep 7, Oct 12, Nov 11, Nov 26, Dec 25 (Jul 4 Saturday: open Jul 3) |
| 2027 | Jan 1, Jan 18, Feb 15, Mar 26, May 31, Jun 18, Jul 5, Sep 6, Oct 11, Nov 11, Nov 25, Dec 24 (Dec 31 early close only) | = SIFMA list | Jan 1, Jan 18, Feb 15, May 31, Jul 5, Sep 6, Oct 11, Nov 11, Nov 25 (Jun 19, Dec 25 Saturdays) |

### USD.2 SOFR (index `USD-SOFR`)

| value | registry | source |
|---|---|---|
| day count | ACT/360 | "actual number of calendar days, but assuming a 360-day year" [4] |
| publication | ~8:00 a.m. ET, T+1 | "Each business day, the New York Fed publishes the SOFR ... at approximately 8:00 a.m. ET" [3]; "SOFR is published on a T+1 basis and the rate for a given day is published at 8:00 a.m., New York City time, on the following U.S. Government Securities Business Day" [5] |
| publication days | calendar `USD-SOFR` | [4][5] (USD.1) |
| rounding | nearest basis point | [4] |
| SOFR Averages / Index | daily compounding, calendar-day weights, /360 | "each SOFR value multiplied by the number of calendar days it covers, divided by 360" [4] |

### USD.3 SOFR OIS (`USD-SOFR-OIS`)

| value | registry | source |
|---|---|---|
| fixed leg | annual, ACT/360 | CFTC MAT: "Fixed Leg Payment Frequency Annual, Day Count Convention ACT/360" [8]; Strata: "Both legs pay annually and use day count 'Act/360'" [9] |
| float leg | annual, ACT/360, `USD-SOFR-OIS Compound` (plain compounding in arrears) | CFTC MAT: "Floating Leg Payment/Reset Frequency Annual, ACT/360", "Fixing Offset 0 Days" [8]; ISDA: "USD-SOFR-OIS Compound" [5, §3] |
| tenors ≤ 1Y | one period (TERM) both legs | Strata `USD_FIXED_TERM_SOFR_OIS` "for terms less than or equal to one year" [9] |
| spot lag | 2 (both USGS and NY business days) | "the Effective Date to be the day that is two business days following the Trade Date" [6]; CFTC MAT "Spot Starting (T+2)" [8] |
| payment delay | 2 business days | "Market convention for SOFR swaps is to apply a two business day payment delay at the end of the Calculation Period" [5, fn 8]; CFTC MAT "Payment Lag 2 Days" both legs [8]; ARRC: standard SOFR OIS "use a payment delay to settle 2 days after the end of the interest period (often referred to as 'T+2')" [10]; Strata "the payment date offset is 2 days" [9] |
| calendars | swap dates `USD-SOFR-SWAP`; fixing `USD-SOFR` | [6]; CFTC MAT "Business Calendars New York/USNY", "Fixing Calendars US Government Securities/USGS" [8] |
| observation shift / lookback / lockout | none (plain) — the interdealer standard | [5, §3 and fn 7 "Compounding with Lookback, Compounding with Observation Period Shift and Compounding with Lockout" are the ISDA variants; the OIS FRO uses none] |
| business day convention | Modified Following | Strata [9]; SwapEngine note — **UNVERIFIED** against a primary SOFR-specific text |
| end-of-month, stub | EOM when the effective date is a month end; short front stub | 2006 ISDA Definitions 4.11 "EOM" — **UNVERIFIED** as the SOFR OIS default (own research) |
| standard tenors | 1W, 2W, 3W, 1M–11M, 1Y, 18M, 2Y–10Y, 12Y, 15Y, 20Y, 25Y, 30Y, 40Y, 50Y | MAT: "2, 3, 4, 5, 6, 7, 10, 12, 15, 20, 30 Years" [8]; the rest commonly quoted — **UNVERIFIED** by fetch |

Compounding mechanics (code, `rfr.hpp`), from the ARRC definitions [10]: payment delay ("interest is paid k days after
the start of the next period"); lockout ("the SOFR rate applied for the last k days of the interest period is frozen
at the rate observed k days before the period ends"; "A 2-5 day lockout has been used in some SOFR FRNs"); lookback
without observation shift ("the observation date is k business days prior to the interest date ... all other elements
of the calculation are kept the same"); lookback with observation shift ("applies that rate for the number of calendar
days until next business date following the observation date"). The ARRC recommends the shifted variant for FRNs
("Two-Day Backward Shifted Observation Period and No Lockouts") and the unshifted one for loans [10].

Variants in the registry for the Stage A trade population (labelled customised, `cross_check: own_research`):
`USD-SOFR-OIS-SHIFT2` (2-day observation shift) and `USD-SOFR-OIS-SHIFT2-LOCKOUT2` (2-day shift and 2-day lockout).
ARRC: "dealers may be able to offer customized over-the-counter derivatives with lockouts to facilitate client
hedging" [10].

SOFR arithmetic-average swaps (`USD-SOFR-AVG-SWAP`): "Overnight Averaging" of the 2021 ISDA Definitions / the
USD-SOFR Average FROs [5, §2]; the average is calendar-day weighted on ACT/360 as for SR1 [12]. Frequencies and lags
by analogy with `USD-SOFR-OIS` — the interdealer standard for SOFR averaging swaps is **UNVERIFIED** (SwapEngine's
`USD-FF-SOFR-BASIS` averages Fed Funds quarterly; that is a different product).

### USD.4 SOFR futures

| contract | reference period | settlement | source |
|---|---|---|---|
| CME SR3 (`USD-SOFR-3M-FUTURE`) | "the interval from the 3rd Wednesday of the 3rd month preceding the delivery month to, but not including, the 3rd Wednesday of the delivery month" (e.g. 91 days) | "100 minus the compounded SOFR rate over the contract reference period", "business-day compounded SOFR per annum", cash-settled, $2,500 × IMM index; "nearest 39 March-quarterly contracts" | [11][13] |
| CME SR1 (`USD-SOFR-1M-FUTURE`) | the calendar delivery month | "100.0000 minus average daily SOFR during the contract delivery month", "arithmetic average", "rounded to the nearest 1/10 of one bp"; last trading day "the last business day of the contract delivery month" | [12] |

The compounding of SR3 is the SOFR Index compounding (calendar-day weights, /360) [4]. SR3's last trading day (the
exchange day before the quarter's end IMM date) is **UNVERIFIED** by fetch (CME pages timed out); the reference quarter
is the field the curve uses. Convexity: zero, stated as a simplification (D35, `PROBLEM.md` §9).

IMM dates 2026 (test `Date.ImmDates2026`): Mar 18, Jun 17, Sep 16, Dec 16 — the FOMC met on three of them [17].

### USD.5 Front-end instruments

`USD-SOFR-ON-DEPOSIT`: the overnight SOFR fixing as a T+0 one-day deposit (ACT/360) — curve-construction practice
(own research). FOMC decision dates (`cb_schedules.USD`): 2026-01-28 … 2027-09-15 verified [17]; the October /
December dates imported from SwapEngine (as_of 2026-09-08) — **UNVERIFIED** by the fetch summary.

---

## 2. EUR

### EUR.1 TARGET (`EUR-TARGET`)

"the TARGET system will be closed on New Year's Day, Good Friday (Catholic/Protestant), Easter Monday
(Catholic/Protestant), 1 May (Labour Day), Christmas Day and 26 December" from 2002 "until further notice", plus
Saturdays and Sundays [14]. No observance shifts (a weekend holiday is simply not a weekday close). 2026 list (test
`Calendar.Target2026And2027`): 1 Jan, 3 Apr, 6 Apr, 1 May, 25 Dec, 26 Dec (Sat) — Bundesbank "TARGET holidays (applies
to all TARGET component systems)" [15]. 2027 from the rule: Fri 1 Jan, Fri 26 Mar, Mon 29 Mar (1 May, 25 and 26 Dec
fall on the weekend) — no published 2027 list fetched (**UNVERIFIED** as a list).

### EUR.2 €STR and EURIBOR

| index | value | source |
|---|---|---|
| `EUR-ESTR` | overnight; ACT/360; published each TARGET2 business day for the previous business day (T+1), three decimals; page shows 08:00 CET, the March 2021 methodology text "no later than 09:00 CET" | [16][18]; ACT/360 from the OIS templates [19][9] |
| `EUR-EURIBOR-3M`, `-6M` | term; ACT/360 (the only official basis since 2018-12-03: "Euribor under its official Act/360 day count convention is not affected"); fixing **2 TARGET business days before the period start** ("The first Euribor Fixing Date is 2 Target business days prior to the Effective Date"; LCH "Fixing T-2"); tenors 1W, 1M, 3M, 6M, 12M after the cessation of 2W, 2M, 9M | [20][21][22] |

The 11:00 CET EURIBOR publication time is **UNVERIFIED** (not in the harvested EMMI pages).

### EUR.3 €STR OIS (`EUR-ESTR-OIS`)

| value | registry | source |
|---|---|---|
| fixed leg | annual, ACT/360 | TP ICAP MET template: "Payment Frequency: Annually", "Fixed Rate Day Count Fraction: A/360" [19]; Strata "Both legs pay annually and use day count 'Act/360'" [9] |
| float leg | annual, ACT/360, `EUR-EuroSTR-OIS Compound`, reset "Annually/The last Business Day of each Calculation Period" | [19] |
| tenors ≤ 1Y | one period | Strata `EUR_FIXED_TERM_ESTR_OIS` [9] |
| spot lag | 2 | "Market standard is Trade Date plus two Business Days" [19]; Strata [9] |
| **payment delay** | **2** — DISAGREEMENT recorded | Strata `EUR_FIXED_1Y_ESTR_OIS`: "the payment date offset is 2 days" (EONIA: 1 day) [9]; SwapEngine: 2 "desk-confirmed; EONIA legacy was 1 BD"; **versus** TP ICAP template "Payment Lag: One Business Day" [19] and LCH 2019 "Payment lag 1 day" for €STR swaps [22] |
| calendar, bdc | TARGET, Modified Following | "Calc Period Business Days: Target", "Modified Following" [19] |
| eom, stub | as SOFR OIS (**UNVERIFIED**); front and back stubs eligible [22] | |
| tenors | as SOFR OIS; LCH: 7 days to 51 years [22] | quoted set **UNVERIFIED** by fetch |

### EUR.4 EURIBOR swaps (`EUR-EURIBOR-3M-IRS`, `EUR-EURIBOR-6M-IRS`)

| value | registry | source |
|---|---|---|
| **fixed leg** | annual, **30/360 (Bond Basis)** — DISAGREEMENT recorded | CFTC MAT: "Annual", "30/360" (fn 2: 4Y/6Y "Annual and 30/360") [8]; Bloomberg SEF: "Payment: Annual, Day Count Conventions: 30/360" [21]; Strata: "The fixed leg pays yearly with day count '30U/360'" [9]; **versus** SwapEngine 30E/360 ("Eurobond basis"); LCH accepts both [22] |
| float leg | 3M quarterly / 6M semi-annual, ACT/360, fixing 2 TARGET days in advance | CFTC MAT: "Quarterly (3M EURIBOR), Semi-Annual (... 6M EURIBOR)", "Actual/360" [8]; Bloomberg SEF [21] |
| spot lag | 2 | "Spot Starting (T+2)" [8]; "Effective Date is T+2 from the trade date" [21] |
| payment delay | 0 | none in the templates [21]; **UNVERIFIED** against ISDA text |
| calendar, bdc | TARGET, Modified Following | [8][21] |
| tenors | 1Y, 18M, 2Y–10Y, 12Y, 15Y, 20Y, 25Y, 30Y (6M also 40Y, 50Y) | MAT: "2, 3, 4, 5, 6, 7, 10, 15, 20, 30 years" [8]; the rest **UNVERIFIED** |

The two 30/360 variants differ only when a date is the 31st (or by the February rule, which ISDA's 30/360 does not
have): `days_30_360_us` vs `days_30e_360` in `daycount.hpp`, tested.

### EUR.5 3s6s tenor basis (`EUR-3S6S-BASIS`)

Quoting convention: **the spread is on the 3M (shorter) leg**, 6M flat: "a positive spread must be added to the 3M
floating leg to equate the value of the 6M floating leg" [23]; SwapEngine agrees ("Spread on the SHORTER (3M) leg").
Legs as the EURIBOR IRS float legs (quarterly / semi-annual, ACT/360, TARGET, Modified Following, T+2); €STR-discounted.
The spread is paid simple each quarter (own statement). Tenor set 1Y–30Y **UNVERIFIED** by fetch.

### EUR.6 Euribor futures and deposits

Eurex FEU3 (`EUR-EURIBOR-3M-FUTURE`): last trading day "two exchange days prior to the third Wednesday of the respective
maturity month" (= final settlement day, provided EMMI determines the 3M rate that day); "The numerical value of the
EURIBOR rate is rounded to three decimal places and then subtracted from 100"; EUR 2,500 per index point; contract
months "six consecutive calendar months plus 22 quarterly months following the March, June, September, December cycle"
[24]. The underlying deposit runs from the third Wednesday (the fixing's spot date on TARGET) for three months, Modified
Following (own derivation from the EURIBOR spot convention). Convexity zero, stated.

Deposits: `EUR-ESTR-ON-DEPOSIT` (the €STR fixing, T+0 one day), `EUR-EURIBOR-3M-DEPOSIT`, `EUR-EURIBOR-6M-DEPOSIT`
(spot-starting T+2, ACT/360, Modified Following) — the front-end "deposits / fixings" of `PROBLEM.md` §3 [20][21].

### EUR.7 ECB Governing Council monetary-policy decision dates

`cb_schedules.EUR`: 2026-10-29, 2026-12-17 and all 2027 dates (Feb 4, Mar 18, Apr 29, Jun 10, Jul 22, Sep 9, Oct 28,
Dec 16) verified [25]; the 2026 January–September dates imported from SwapEngine (past meetings, **UNVERIFIED** by the
fetch).

---

## 3. Cross-check against the SwapEngine conventions file (D37)

| value | SwapEngine says | source says | verdict |
|---|---|---|---|
| USD SIFMA calendar rules (New Year's, MLK, Presidents, Good Friday except first Friday, Memorial, Juneteenth from 2022, Independence, Labor, Columbus, Veterans sun_to_mon, Thanksgiving, Christmas; observance sat_to_fri_sun_to_mon) | as listed | SIFMA 2026/2027 lists [1] reproduce every date | **agree** (representation differs: SwapEngine's year-bucketing artefact for New Year's on a Saturday is an explicit `sun_to_mon` observance here; `except_first_friday` is the generic `skip_if_first_weekday_of_month`) |
| SOFR calendar: Good Friday never a publication day | "SOFR is not published on Good Friday, jobs report or not" | NY Fed 2026 announcement via ISDA [5] | **agree** (earlier early-close Good Fridays UNVERIFIED) |
| Federal Reserve calendar (Sat not observed, Sun→Mon; Juneteenth from 2021) | as listed | [7] | **agree** |
| TARGET rules | Jan 1, GF, EM, May 1, Dec 25, Dec 26, no observance | ECB [14], Bundesbank 2026 [15] | **agree** |
| joint calendars USD+USD-FED (SOFR swap dates), EURUSD | join | ISDA MPN [6] for the SOFR swap calendar; EURUSD not researched (Stage B) | **agree** / not_in_source |
| SOFR index: ACT/360, calendar USD-SOFR, publication lag 1 | as listed | [3][4] | **agree** |
| €STR index: ACT/360, TARGET, publication lag 1 | as listed | [16][18][19] | **agree** |
| EURIBOR 3M/6M: ACT/360, TARGET, fixing lag 2 | as listed | [20][21][22] | **agree** |
| SOFR OIS: spot 2, payment 2, MF, calendar USD+USD-FED, fixed ACT/360 1Y, float compounded 1Y ACT/360 | as listed | [5][6][8][9][10] | **agree** |
| €STR OIS payment lag | 2 ("desk-confirmed; EONIA legacy was 1 BD") | Strata 2 [9]; TP ICAP 1 [19]; LCH 2019 1 [22] | **disagree among sources**; registry 2; recorded (EUR.3) |
| €STR OIS: spot 2, MF, TARGET, fixed ACT/360 1Y, float compounded 1Y ACT/360 | as listed | [9][19] | **agree** |
| EURIBOR IRS fixed-leg day count | 30E/360 | 30/360 (Bond Basis): CFTC MAT [8], Bloomberg SEF [21], Strata 30U/360 [9] | **disagree**; registry 30/360; recorded (EUR.4) |
| EURIBOR IRS: fixed annual, float 3M quarterly / 6M semi-annual ACT/360, spot 2, payment 0, MF, TARGET | as listed | [8][21] | **agree** |
| 3s6s: spread on the 3M leg, €STR-discounted | as listed | [23] | **agree** |
| SR3: IMM quarter, compounded, ACT/360, 100 − rate | as listed | [11][13] | **agree** (SwapEngine: convexity a per-quote user input; here zero and stated) |
| SR1: calendar month, averaged, ACT/360, 100 − rate | as listed | [12] | **agree** |
| FOMC / ECB meeting dates | as_of 2026-09-08 lists | [17][25] for the dates fetched | **agree** where verified; the rest imported |
| day-count names | ACT/360, 30E/360, 30U/360, ACT/365F, ACT/ACT-ICMA, ACT/ACT-ISDA, BUS/252 | the five implemented here: ACT/360, ACT/365F, 30/360, 30E/360, ACT/ACT ISDA (ISDA 4.16) | agree on names; ICMA and BUS/252 not implemented (no Stage A use) |

Not imported (no Stage A use): Fed Funds index and products, FF/SOFR basis, GBP/JPY, bonds, credit, FX, xccy, inflation.

## 4. UNVERIFIED items (all marked in the JSON citations too)

1. SIFMA's treatment of a **Saturday Veterans Day** (next: 2028-11-11): not observed on the Friday (SwapEngine rule; SIFMA 2023 practice) — no fetch.
2. Whether SOFR was unpublished on earlier early-close Good Fridays (2021-04-02): 2026 is confirmed [5].
3. The SIFMA Good Friday first-Friday rule for the whole 2020–2080 span: a simplification of the BLS calendar.
4. Business day convention (Modified Following), end-of-month rule and short-front-stub default for SOFR / €STR OIS as the interdealer default (Strata and SwapEngine say MF; EOM/stub own research).
5. The quoted tenor sets beyond the CFTC MAT lists (front ends, 8Y, 9Y, 25Y, 40Y, 50Y).
6. SOFR averaging swap frequencies and lags (by analogy with SOFR OIS).
7. SR3 last trading day; SR1 non-business-day weighting (CME pages timed out).
8. EURIBOR 11:00 CET publication time; EUR IRS payment lag 0 against ISDA text.
9. A published TARGET 2027 list (the ECB rule is cited instead).
10. October / December 2026 and 2027 FOMC dates and the January–September 2026 ECB dates (imported).
11. The EURUSD joint calendar (Stage B).

## 5. Sources

1. SIFMA, Holiday Schedule — U.S. Holiday Recommendations, 2026 and 2027 tabs. https://www.sifma.org/resources/guides-playbooks/holiday-schedule (read 2026-09-23; the 2027 tab from the page's embedded data); SIFMA press release 2025-12-17 "SIFMA Issues 2026 and 2027 Fixed Income Recommendations for Full & Early Holiday Closes in the U.S., U.K., and Japan".
2. 2021 ISDA Interest Rate Derivatives Definitions, "U.S. Government Securities Business Day" (as quoted in [5]).
3. Federal Reserve Bank of New York, Secured Overnight Financing Rate Data. https://www.newyorkfed.org/markets/reference-rates/sofr
4. Federal Reserve Bank of New York, Additional Information about Reference Rates Administered by the New York Fed. https://www.newyorkfed.org/markets/reference-rates/additional-information-about-reference-rates
5. ISDA, Guidance regarding the publication of SOFR on Good Friday, April 3, 2026 (2026-03-27), citing NY Fed operating policy 260312a (harvested PDF).
6. ISDA, Market Practice Note: Effective Dates for SOFR swaps using different Payment/Reset Date calendars (2022-04-08) (harvested PDF).
7. Federal Reserve Financial Services, Holiday Schedules. https://www.frbservices.org/about/holiday-schedules
8. CFTC, Swaps Made Available To Trade — summary chart as of 2023-07-07 (harvested PDF).
9. OpenGamma Strata, `StandardFixedOvernightSwapConventions.java` and `StandardFixedIborSwapConventions.java` (main branch, raw.githubusercontent.com, read 2026-09-23); API docs `FixedOvernightSwapConventions`.
10. ARRC, A User's Guide to SOFR (2021), Technical Appendices (harvested PDF); ARRC, SOFR In-Arrears Securitization Conventions Matrix addendum (Nov 2021).
11. CME Group, A Practitioner's Guide to Three-Month SOFR Futures Contract Notional; CME, Three-Month SOFR Futures Contract Specs (page timed out; facts via the search engine's excerpt of those pages).
12. CME Group, Rulebook Chapter 461 One-Month SOFR Futures; One-Month SOFR Futures Contract Specs (via the search engine's excerpt).
13. SR3 contract specification summary (metrotrade help centre, mirroring CME): contract unit, listing, tick, settlement. https://help.metrotrade.com/kb/three-month-sofr-futures-sr3-contract-specifications
14. ECB press release 2000-12-14, TARGET closing days from 2002. https://www.ecb.europa.eu/press/pr/date/2000/html/pr001214_4.en.html
15. Deutsche Bundesbank, Public holidays in Germany in 2026 (TARGET holidays) (harvested PDF).
16. ECB, Euro short-term rate (€STR). https://www.ecb.europa.eu/stats/financial_markets_and_interest_rates/euro_short-term_rate/html/index.en.html
17. Federal Reserve Board, FOMC Meeting calendars. https://www.federalreserve.gov/monetarypolicy/fomccalendars.htm
18. ECB, The euro short-term rate (€STR) methodology and policies (harvested PDF).
19. TP ICAP, IRP MET template — EUR Overnight Index Swap (2021 ISDA Definitions) (harvested PDF).
20. EMMI, EMMI to extend the publication of Euribor under the Act/365 and 30/360 count basis until 31 March 2019 (D0457A-2018, 2018-11-28) (harvested PDF); EMMI Euribor methodology (harvested PDF).
21. Bloomberg SEF LLC, CFTC submission 2013-09-P15 (2013-09-30): EUR EURIBOR interest rate swap contract specifications (harvested PDF).
22. LCH, Product Eligibility EUR (October 2019) (harvested PDF).
23. M. Bianchetti, M. Carlicchi, Markets Evolution After the Credit Crunch, arXiv:1301.7078 (harvested PDF).
24. Eurex, Three-Month EURIBOR Futures (FEU3) contract specifications. https://www.eurex.com/ex-en/markets/int/mon/euribor-derivatives/euribor/Three-Month-EURIBOR-Futures-137458
25. ECB, Governing Council meeting calendar. https://www.ecb.europa.eu/press/calendars/mgcgc/html/index.en.html

---

## 6. Instrument mechanics and curve definitions (M3/G2, D43)

The coupon mechanics of `include/epykos/maths/instrument/coupon.hpp` are code (D36); every convention they apply
comes from the registry entries above. What the code implements, with the sources it follows:

| mechanism | formula as coded | source |
|---|---|---|
| compounding in arrears | `Π (1 + r_i · n_i / basis)` over the observation days, `R = (Π − 1) · basis / (calendar days of the observation period)`, the coupon `N · τ_accrual · (R + spread) · DF(pay)` | 2021 ISDA Definitions §7.3 OIS compounding formula as cited in [5]; FRBNY SOFR Averages / Index compounding "each SOFR value multiplied by the number of calendar days it covers, divided by 360" [4]; the ARRC formulas for payment delay, lookback, observation shift and lockout [10] (the windows are `conventions/rfr.hpp`, USD.3) |
| the projected rate for a day | the overnight forward over the rate's own span `[r, r')`, `r'` the next fixing business day, applied for the entry's weight (plain: span = weight, the product telescopes) | the definition of an overnight rate FOR a day, [3][4][16]; own statement of the projection |
| arithmetic average | `Σ r_i · n_i / (calendar days)`, calendar-day weighted, the daily forwards for the projected days | ISDA Overnight Averaging / USD-SOFR Average FROs [5, §2]; CME SR1 "arithmetic average of daily SOFR values during the contract delivery month" [12] |
| term rate fixing in advance | the fixing of `fixing_lag` business days before the accrual start when it is in the history (realised), else the forward over the accrual period with the index's day count | EURIBOR fixing T−2 [21][22]; the stub forward is NOT interpolated between index tenors — a stated simplification |
| futures price | `100 · (1 − rate)`; SR3 = the compounded SOFR over the IMM quarter, SR1 = the average over the month, FEU3 = the 3M fixing of the last trading day (the deposit from the third Wednesday) | [11][12][13][24]; convexity adjustment zero, stated (D35, `PROBLEM.md` §9) |
| deposit | a one-period swap: the fixed coupon at the deposit rate against the index forward over the same period; `par` = the forward | own construction (USD.5, EUR.6) |
| 3s6s basis | the spread paid simple on the quarterly 3M leg, the 6M leg flat; `par spread = (PV 6M leg − PV 3M leg at the forwards) / annuity(3M leg)` | Bianchetti–Carlicchi [23]; EUR.5 |
| accrued interest | the rate known so far (fixed rate, realised fixing, realised compounded / averaged rate) × the accrual from the period start to the valuation date | own statement (an O2 output of `PROBLEM.md`) |

Curve definitions (`blueprints/curves/usd.json`, `eur.json`) take their tenor sets from the registry's cited
standard tenors (USD.3, EUR.3, EUR.4, EUR.5) and the front-end instruments from `PROBLEM.md` §4 (deposits / fixings
and futures on the front year, convexity zero): USD-SOFR = the overnight fixing, OIS 1M / 2M / 3M (bridging the
fixing and the first IMM quarter — own construction, the exact front-end set is item 5 of section 4), eight SR3
quarters, OIS 3Y–30Y, on four schemes (linear zero, log-DF, monotone cubic, a composite with regions at the 3Y and
15Y knots); EUR-ESTR = the fixing and €STR OIS 1M–30Y; EUR-EURIBOR-6M = the 6M deposit and fixed-vs-6M swaps 1Y–30Y
(the standard EUR IRS [8][21]); EUR-EURIBOR-3M = the 3M deposit, eight FEU3 contracts and 3s6s basis swaps 3Y–30Y
against the 6M curve [23] — the multiple-curve bootstrapping of the 3M curve through basis swaps. Knots sit at the
instruments' last cash-flow times. Futures notionals in the blueprints are the contract value at 100 points (SR3 /
SR1: USD 250,000 = USD 2,500 per point [13]; FEU3: EUR 250,000 = EUR 2,500 per point [24]).

No SwapEngine file was opened for G2: every convention came from the registry G0 imported and cross-checked (D37).

