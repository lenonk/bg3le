# Looking for `ls::GlobalSwitches`

**Solved 2026-09-25.** The global is at image+0x7d9d198. The engine's settings
registration (image+0x3f5bd10) reads 102 of the switch names from their
string-view table (image+0x78c6ca8) alongside 60 loads of that global, and
pairing each name with the member it reads gives this build's offset for 46
declared members. bg3se's layout matches all 46 once a `SoundSetting` is 0x68
bytes (an eight-byte member before `UpdateProc`) and eight bytes follow
`SomeSettings`; the camera block, whose values read as the settings they are
named for (`UpperBound` 10, `LowerBound` -10, `CollisionPitchSearchSteps` 6),
needed nothing. `src/vendor/global_switches.cpp` checks the loading
instruction and the language string before trusting the address. The
language anchor below was right about the object; what defeated the old
search was the two size differences, which no scoring of booleans or floats
could have told apart from a wrong base.

The rest of this page is the earlier search, kept for the method.


Written 2026-09-24. The object is findable; reading it through bg3se's
declared layout is not safe, so `Ext.Utils.GetGlobalSwitches` still refuses —
but it refuses on evidence now, and `src/vendor/global_switches.cpp` is the
search that produced it.

It matters twice over: it is one refusing `Ext.*`, and it is also the second
thing blocking bg3se's ImGui overlay, whose `InitializeUI` reads
`GetGlobalSwitches()->Language`.

## The anchor works

`Language` is one of Larian's sixteen-byte strings holding a short value, so
an English install has exactly this somewhere in it:

    45 6e 67 6c 69 73 68 00 00 00 00 00 00 00 00 07
    E  n  g  l  i  s  h                          len

Fifteen characters inline and the length in the last byte — a sixteen-byte
needle with no wildcards, which is far better than scanning for the text. A
scan finds 130–190 of them across the process, so a hit is a candidate: the
object would start `offsetof(Language)` before it.

## Boolean density is not a test

`GlobalSwitches` declares 91 boolean members, and a bool is 0 or 1. That
looked like a strong check — 91 independent one-bit tests — and it is not,
because a settings object is not the only thing made of small bytes. Accepting
nine in ten gave a base with 82 of 91 agreeing whose scalars were plainly
wrong:

    UIScaling          1060
    MouseSensitivity   -1158458304        <- a float's bits read as an int
    MaxNrOfAutoSaves   0
    CanAutoSave        false

Requiring all 91 rejected everything. Between runs the best score moved
between 66, 82 and 87 at different addresses, which is what a test with no
discriminating power looks like.

## Floats are a test

A float is thirty-two bits and almost all of them are meaningless. A setting
is a small finite number; random bytes are overwhelmingly NaN, infinite,
denormal or astronomical. `GlobalSwitches` declares twenty-odd floats —
`FadeSpeed`, `GameCameraRotation`, the camera speeds and the controller
thresholds — and requiring every one to be finite with a magnitude between
1e-6 and 1e6 is a real filter.

With both tests applied, no candidate in the process passes. The best after
the float filter scores 66 of 91 booleans.

## What that means

bg3se's `GlobalSwitches` is a Windows reverse-engineering: two thirds of its
members are named `field_NN`, and it contains `TranslatedString`, `HashSet`
and `STDString` members whose sizes differ on this build. So the declared
offsets are not this struct's offsets, and an address reported against them
would read the wrong fields — the "plausible wrong answer" this project
refuses to give.

One detail is worth keeping for whoever picks this up: the disagreeing
booleans are consistent across runs and start at the same place.

    +208   ShowLocalizationMarkers
    +212   EnablePortmapping
    +228   CrossplayEnabled
    +229   CrossplayInUse

The same four, run after run, at the same offsets, on the candidate whose
language reads "English". That consistency says something structural sits just
before +208 with a different size here, rather than that the search is finding
noise. Establishing what, the way `STDString` and `Module` were established,
is the way in — and it would want a live dump of the bytes around a confirmed
base, which needs a confirmed base first.

## Solving for the drift, and why the answer was rejected

Written 2026-09-24. The disagreements starting consistently at +208 looked
like a member before that point having a different size on this build, with
everything after it shifted by a constant. That is a solvable shape, so
`solve_layout` was written to solve it: take the lowest offset that
disagrees, try shifting every offset from there up by a constant, keep the
shift that agrees best, and repeat on what is left.

With six breaks allowed it solved. All ninety-one booleans and all ten floats
agreed, at:

    everything from +212  shifts by  +36
    everything from +4828 shifts by   +4
    everything from +4988 shifts by -116
    everything from +5033 shifts by  -84

That is not a struct. It is a curve fit, and recording it is the point: each
break gives the search sixty-four free values, a boolean check is one bit,
and two negative shifts of eighty bytes and more are not what a member
changing size looks like. The solver is capped at one break now, which is a
claim that can be wrong -- one member before the pivot differs in size,
everything after it moved by that much, nothing else changed. One break does
not solve it.

## Strings are a better test than booleans, and there are not enough of them

A boolean is one bit. A float is thirty-two, most of whose patterns are NaN
or astronomical. One of Larian's strings is a hundred and twenty-eight bits
with a length that has to agree with its own contents: inline, the top bit of
the last byte is clear, that byte is the length, and everything from the
length to the terminator is zero; on the heap, the top bit is set and there
is a readable pointer with a size no larger than its capacity and a
terminator where the size says. `looks_like_string` in
`src/vendor/global_switches.cpp` tests exactly that.

`GlobalSwitches` declares seven strings -- `Language`, `ScreenshotDir`,
`EBSUrl`, `TwitchExtSecret`, `TwitchExtSecret2_M`, `field_158`,
`LongRestDefaultTimeline` -- but bg3se's property map exposes only four of
them, at +0, +48, +168 and +4816 from `Language`. And an empty string is
sixteen zero bytes, which passes for free: the first version of this counted
seventy-seven "strings" in a kilobyte of a settings object for that reason.
So the test is worth one or two real checks rather than four.

**No candidate in the process passes it.** Across 162 hits on the language
needle, not one has the other three declared strings where bg3se says they
are. That is the firmest evidence so far, and it is worse news than the
boolean count was: `ScreenshotDir` is only +48 from the anchor, so the drift
begins within a few members of `Language` rather than thousands of bytes
away.

## What was tried and does not work

- **Boolean density.** Recorded above; 82 of 91 at a base whose `UIScaling`
  read 1060.
- **Shifting the offsets to fit.** Solves with four arbitrary breaks. Any
  test with that much freedom will solve.
- **Finding the object by its string cluster.** Searching forward from each
  language hit for the densest run of non-empty strings finds arrays of
  strings -- runs at a clean sixteen-byte stride -- not a settings object
  with strings scattered through it. Density is the wrong signal.

## What would work

A different anchor. Everything above starts from the language string and
tests bg3se's offsets against it, and bg3se's offsets are wrong near the
anchor. The way in is a pointer to the object from something already located,
the way `ModManager.Settings` was found at +112 -- the engine reaches
`GlobalSwitches` from somewhere, and that somewhere is a better starting
point than a string in the middle of it.

## What the code does now

`bg3le_global_switches()` runs the search once, requires every float to be
plausible and every boolean to be 0 or 1, and returns null otherwise. On
failure it logs the best candidate, its language, its score and the
disagreeing members by name and offset, so the next attempt starts from a
measurement rather than from scratch.
