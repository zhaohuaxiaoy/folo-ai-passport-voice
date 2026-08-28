---
name: folotoy-ai-passport-publisher
description: Prepare, submit, update, and check FoloToy AI Passport community firmware projects through the official publisher API. Use when a creator asks to publish or maintain an AI Passport firmware project; do not use for ordinary firmware development or flashing.
---

# FoloToy AI Passport Publisher

Prepare a trustworthy community submission from the creator's current project, then use the bundled publisher script for authorization and upload.

## Workflow

1. Inspect the project README, documentation, source, Git remote, build configuration, and existing artifacts. Identify a single merged ESP `.bin` image intended to flash from `0x0`; do not guess between ambiguous builds.
2. Prepare both Chinese and English titles and descriptions using the play-first editorial rules below. Translate and improve existing project copy when one language is missing. If there is no usable description, infer a concise factual draft from the README, documentation, screenshots, demo media, user-facing strings, and observable interaction flow; clearly flag anything uncertain instead of inventing features.
3. Resolve the current source repository from the Git remote when one exists. GitHub, Gitee, GitLab, Codeberg, and other publicly reachable HTTPS Git repository pages are supported. Convert an SSH remote to its corresponding HTTPS project page when unambiguous. A repository URL is optional; do not block submission when the project has no public repository.
4. Select or prepare a representative JPEG, PNG, or WebP cover. Keep every claim grounded in the project.
5. Run `python3 scripts/publisher.py validate --firmware <file> --cover <file>` before proposing a submission.
6. If authorization is missing, run `python3 scripts/publisher.py authorize`. The official creator page opens automatically. Guide the creator to register with email or sign in there, then approve the displayed code. Never ask for, receive, or store their password.
7. Run `submit` with `--source-url` when available and without `--confirmed` to print the exact preview. Show the user every field, both file paths, size, and SHA-256. Obtain explicit approval immediately before uploading.
8. Only after approval, rerun the same command with `--confirmed`. Report the returned project, revision, slug, and review status.

For an update, run `projects` first and match the existing project by ID and slug. Use `submit --project-id <id>` only after confirming the target with the user. Never create a second project merely because an update failed.

## Play-first editorial rules

Public copy is for people choosing something fun to use, not for reviewing a hardware design:

- Explain what the application turns AI Passport into, how someone starts or plays, and what makes the experience interesting, surprising, social, useful, or delightful.
- Name the play or application in the title. Do not name the board, chip, component, framework, or hardware solution.
- Keep the description grounded in behavior that can be verified from the project. Prefer a simple flow such as: what it is, how to play, and why it is worth trying.
- Read technical files when needed to find and validate the correct firmware, but do not carry implementation details into public titles or descriptions.
- Exclude chip and board names, vendors, audio codecs, display controllers or specifications, buses, pins, memory and flash sizes, partition details, SDKs, frameworks, build systems, and similar engineering terminology. Examples that must not appear in public copy include `ESP32`, `ESP32-C3`, `ESP32-S3`, `Espressif`, `乐鑫`, `ES8311`, `I2C`, `I2S`, `SPI`, `UART`, `GPIO`, `OLED`, `LCD`, `LVGL`, and screen model or resolution details.
- If the repository is mostly technical, derive the experience from its usage steps, screenshots, demo videos, assets, interface text, and runtime behavior. Ask the creator for the missing gameplay only when it cannot be established without guessing.
- Write the English version as natural product copy with the same meaning and tone, not as a literal translation.

Before showing the submission preview, reread both descriptions and rewrite any technical specification as a user-visible benefit or interaction. Do not merely delete terminology if that leaves a vague or broken sentence. For example, rewrite “基于 ESP32-S3、ES8311 和 2.4 英寸屏幕的离线互动播放器” as “把 AI Passport 变成随身互动播放器，点开角色就能重温经典片段，适合聚会时和朋友一起玩。”

## Boundaries

- Upload only to `https://ai-passport.folotoy.cn` unless the user explicitly requests a development server. Use `FOLOTOY_AI_PASSPORT_URL` for that override.
- Firmware must be a non-empty ESP merged image no larger than 8 MiB. Covers must be JPEG, PNG, or WebP no larger than 10 MiB. When supplied, the source must be a public HTTPS Git repository project page, not a local path or credential-bearing URL.
- Authorization grants submission access only. Credentials are stored outside the project with owner-only permissions; never print, commit, or copy the token into source files.
- Publishing and updating are external mutations. Validation, drafting, and preview do not authorize upload.
- Do not approve moderation, fabricate engagement, or bypass server validation.

Read [references/api.md](references/api.md) only when troubleshooting authorization or server responses.
