# Publisher API

The bundled script is the preferred interface. Use these details only to diagnose a failure.

## Authorization

- `POST /api/agent/device-code` creates a ten-minute browser authorization request.
- Open `verificationUriComplete`; the creator signs in or registers and approves the displayed code.
- Poll `POST /api/agent/token` with `device_code`. HTTP 428 means authorization is still pending.
- The returned bearer token is scoped to creator submissions, expires after 30 days, and can be revoked from the creator page.

## Creator operations

Bearer authentication uses `Authorization: Bearer <token>`.

- `GET /api/agent/me` verifies the authorization.
- `GET /api/agent/projects` lists the creator's latest project revisions.
- `POST /api/agent/submissions` creates a project.
- `POST /api/agent/submissions/{project_id}/resubmit` submits a revision.

Submission endpoints accept multipart fields `title_zh`, `description_zh`, optional `title_en`, `description_en`, and `github_url`, plus `cover` and `firmware`. The historical optional `github_url` field name accepts GitHub, Gitee, GitLab, Codeberg, or another public HTTPS Git repository page. Server validation remains authoritative.

Do not retry a rejected mutation automatically. Show the response to the creator and resolve the cause first.
