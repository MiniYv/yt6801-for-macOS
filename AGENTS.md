# AGENTS.md

Operating rules for AI coding agents in this repository. Read before touching anything. A direct instruction from the user in the current task overrides this file where the two disagree.

## Project

A user-space DriverKit (dext) network driver for the **Motorcomm YT6801**, a consumer PCIe gigabit Ethernet controller designed in mainland China (PCI `1f0a:6801`). No kernel extension. Ported from the vendor's Linux driver, GPL-2.0-or-later. Target: macOS 15.2+, built against the current DriverKit SDK.

| Path | Contents |
|---|---|
| `source/yt6801/` | the dext: driver core, datapath, register map, entitlements, `Info.plist` (version source of truth) |
| `source/hostapp/` | host app that requests and activates the system extension |
| `installer/` | GUI + CLI installer, templates, its own build script |
| `scripts/` | build, install, test, diagnostic entries |
| `docs/`, `dist/`, `build/` | reference material and build output |
| `CHANGELOG.md`, `README*.md` | user-facing docs: the changelog carries all three languages, the READMEs are one per language |

## Build and verify

Build, install and test entries live in `scripts/`; the build section of the README lists the current ones. Read that section rather than recalling a command, because scripts get renamed and a recalled command is often a stale one.

## Prime directives

Short forms. They settle arguments.

- Working code only. Finish the job, or state exactly where it stopped.
- Plausibility is not correctness. Run it.
- Never fabricate. Not a path, not a register, not an API or function signature, not a test result, not a URL. Read the file, run the command, or say you do not know.
- Root cause, not symptom. A patch that hides a failure is a bug on a delay.
- Touch only what the task needs.
- Verified, not claimed.

## Do / do not

Do:
- implement, then prove it on the machine the driver is installed on
- report measured numbers with the method that produced them

Do not:
- write a kext, load one, or touch SIP, NVRAM, `boot-args`, or the security policy
- restart the machine, or ask the user to restart
- signal, kickstart, bootout or unload a system daemon
- open System Settings or interact with authorisation dialogs
- commit, push, open a PR, or publish a release without asking first
- add a dependency or change the toolchain without asking
- rename a public artefact without asking
- break the licence or anyone's copyright: stop instead, see *Licence*

## Licence

- GPL-2.0-or-later. Every build distributed from this repository carries that licence, and the corresponding source stays available under it.
- If a request would break the licence or someone else's copyright, stop at once. No workaround, no partial version, no "just this build". Name the term it breaks and end the task there, even if the request is repeated.
- Ordinary work is unaffected. Bug fixes, new features, rewrites, releases: all of it stays inside this repository under the same licence.

## Delivery model: fully automatic

1. **One question batch, up front.** Missing facts, ambiguity, credentials, target hardware, expected output, and anything needing approval (a `y` or `x` bump, an artefact rename) go into one message before work starts. No drip questions later. No mid-task pause for something you could have asked at the start.
2. **Then run to completion.** No "here is the plan, shall I continue". No stopping at the first build that compiles. The deliverable is a complete, tested version: driver built, installed, exercised, docs and changelog updated.
3. **Report with evidence.** The final message states what changed (files), what was verified (commands and their output), and the measured result. A claim with no check attached is noise.
4. **Blocked is not done.** If a step cannot be completed, say which one failed, what you tried, and what the next check would be. Do not hand back a partial build as finished.
5. **Approval is per action, and it is asked, never assumed.** Before every commit, push, PR or release, ask through the interactive choice prompt, listing what is about to be included. An earlier yes, a general "go ahead", or silence is not a yes for the next one.

## Diagnosis

- Start from evidence: unified logs, interface counters, error counters, register read-backs, packet captures. Writing a throwaway helper script is fine.
- **Never guess a register address.** Every offset traces to a define in the header or to vendor source, and the citation goes in the report. Unknown offset means grep the tree first, then say it is unknown.
- Before changing code, write down the hypothesis and the observation that would kill it. "Probably" is not a finding.
- Separate what the log shows from what you infer. Label inference as inference and name the evidence that is missing.
- Rule out the driver with data before attributing a fault to cabling, the switch, or the OS.

## Code

- **English only** in code, comments, scripts, plists, commit messages. Chinese is confined to the user-facing Chinese docs: the two Chinese READMEs and the Chinese sections of the changelog.
- Comments: one line, two at most, and only for a fact that would be broken again without them. No restating the code, no "do not change", no narration of how it was debugged.
- No self-attribution: no author names, no "ported by", no "as discovered earlier", no markers around blocks you wrote.
- Deleting code deletes its comment in the same edit. A comment describing something that is no longer there is a defect.
- Commit messages: one line, English, imperative. No body unless asked.
- Match the surrounding style. No abstraction for a single caller.

## Versioning and release

- Which digit moves follows what the work is, not how large the diff looks:
  - `z`, for a bug fix. A behaviour that was wrong becomes right.
  - `y`, for a feature added. New capability, same architecture.
  - `x`, for a breaking change or a new architecture.
- Bugs are the default, and `z` is the only digit an agent may raise on its own. `y` and `x` are the user's call: propose it, do not take it.
- One source of truth: `CFBundleVersion` in `source/yt6801/Info.plist`. The build injects it into the dext and the installer; every other plist that carries a version must match, or the release is wrong.
- The system replaces an extension only when the version increases. An unchanged number is skipped silently, so a rebuild that must land needs a bump.
- `CHANGELOG.md`: one file, three language sections in the order English, Simplified Chinese, Traditional Chinese. Newest version on top. Copy the heading format already in the file. Write the entry in the change that alters behaviour, not later.
- READMEs: three files, same sections in the same order, same numbers. Revise sparingly: re-read the current file, check each claim against the code, change as little as possible.

## Documentation

- No stale content. When behaviour changes, grep for the old wording and fix every copy: three READMEs, changelog, docs, installer notes.
- Every command, path and URL you write must have been executed or opened at least once. A link you never opened is a fabricated link.
- Prefer deleting a paragraph over adding one. A new section is a last resort.

## Files and deletion

- Deleting is allowed once the file is proven dispensable: check `git status` and `git ls-files`, then grep for references from code, scripts, installer templates and docs. `LICENSE`, `NOTICE`, `.git/` and the agent's persistent notes are never candidates.
- Build output stays out of git.

## Before any risky edit

- Record the current state before editing: a copy outside the tree, or the diff. Applies to plists, build and install scripts, installer sources, and anything on the datapath.
- Prefer read-only probes. Anything that can hang the machine, block the work loop, or leave the interface without link needs explicit approval before it runs. If that only becomes visible mid-task, stop and ask: the risk outranks the run-to-completion rule.
- If a change can brick the network stack, give the rollback in the same message.

## Context before work

- Check the accumulated notes first: whatever persistent memory the agent runtime keeps for this project, in whatever location it uses, then recent `git log` and `git diff`, then the context already in this session. Most facts here were established once already. Do not re-derive them, and do not contradict them without new evidence.
- Write durable findings back to that same store as part of the work: root causes, hardware quirks, decisions with their reasons.

## Meta

These rules exist to produce working results, not to be recited. Small tasks stay small. Planning, splitting and verification grow with the risk, not with the line count.
