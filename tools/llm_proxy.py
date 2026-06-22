#!/usr/bin/env python3
"""
Host-side AgentOS LLM proxy.

The xv6 side prints one-line requests wrapped by:
  @@AGENTOS_LLM_REQ ... @@END

This proxy can run in demo mode today, or call a third-party, OpenAI-compatible
HTTP API in api mode. Secrets and network access stay on the host; xv6 only
sees compact protocol lines.
"""

import argparse
import json
import os
import re
import sys
import urllib.error
import urllib.request


REQ_RE = re.compile(r"@@AGENTOS_LLM_REQ\s+(.*?)\s+@@END")


def field(payload: str, name: str, default: str = "") -> str:
    prefix = name + "="
    for part in payload.split():
        if part.startswith(prefix):
            return part[len(prefix):]
    return default


def demo_model_response(request_id: str, role: str, prompt: str) -> str:
    _ = role
    _ = prompt
    return (
        f"@@AGENTOS_LLM_RESP id={request_id} state=done "
        "text=plan_patch_test action=start_codelab @@END"
    )


def compact_text(text: str) -> str:
    text = text.encode("ascii", "ignore").decode("ascii")
    text = text.strip().replace("\n", "_").replace("\r", "_")
    text = re.sub(r"\s+", "_", text)
    text = re.sub(r"[^A-Za-z0-9_.:-]", "_", text)
    text = text.replace("@@END", "END")
    return text[:48] or "empty_model_response"


def choose_action(text: str) -> str:
    normalized = text.lower()
    if "action=abort" in normalized or "abort" in normalized or "cannot_fix" in normalized:
        return "abort"
    if (
        "action=start_codelab" in normalized
        or "start_codelab" in normalized
        or "fix" in normalized
        or "patch" in normalized
        or "test" in normalized
    ):
        return "start_codelab"
    return "start_codelab"


def call_real_llm_api(
    request_id: str,
    role: str,
    prompt: str,
    api_key: str,
    model: str,
    api_url: str,
) -> str:
    """
    Third-party API integration point.

    Expected API shape: OpenAI-compatible chat completions:
      POST <api_url>
      Authorization: Bearer <api_key>
      {"model": "...", "messages": [...], "temperature": 0}

    Keep secrets and network code on the host side; xv6 only sees this compact
    response line through the serial bridge.
    """
    if not api_key:
        return (
            f"@@AGENTOS_LLM_RESP id={request_id} state=failed "
            "text=missing_AGENTS_LLM_API_KEY @@END"
        )
    if not model:
        return (
            f"@@AGENTOS_LLM_RESP id={request_id} state=failed "
            "text=missing_model @@END"
        )
    if not api_url:
        return (
            f"@@AGENTOS_LLM_RESP id={request_id} state=failed "
            "text=missing_api_url @@END"
        )

    body = {
        "model": model,
        "temperature": 0,
        "messages": [
            {
                "role": "system",
                "content": (
                    "You are the host-side planner for AgentOS CodeLab. "
                    "Decide whether AgentOS should start the CodeLab repair. "
                    "Return exactly: ACTION=start_codelab REASON=<short ASCII reason>. "
                    "If the task is unsafe or unrelated, return ACTION=abort."
                ),
            },
            {
                "role": "user",
                "content": f"role={role}; prompt={prompt}",
            },
        ],
    }
    req = urllib.request.Request(
        api_url,
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
        },
        method="POST",
    )

    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            data = json.loads(resp.read().decode("utf-8"))
    except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
        return (
            f"@@AGENTOS_LLM_RESP id={request_id} state=failed "
            f"text=api_error_{compact_text(str(exc))} @@END"
        )

    text = ""
    try:
        text = data["choices"][0]["message"]["content"]
    except (KeyError, IndexError, TypeError):
        text = data.get("output_text", "") if isinstance(data, dict) else ""

    return (
        f"@@AGENTOS_LLM_RESP id={request_id} state=done "
        f"text={compact_text(text)} action={choose_action(text)} @@END"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", choices=["demo", "api"], default="demo")
    parser.add_argument("--api-key", default=os.environ.get("AGENTOS_LLM_API_KEY", ""))
    parser.add_argument("--model", default=os.environ.get("AGENTOS_LLM_MODEL", ""))
    parser.add_argument("--api-url", default=os.environ.get("AGENTOS_LLM_API_URL", ""))
    args = parser.parse_args()

    for line in sys.stdin:
        match = REQ_RE.search(line)
        if not match:
            continue
        payload = match.group(1)
        request_id = field(payload, "id", "0")
        role = field(payload, "role", "planner")
        prompt = field(payload, "prompt", "")
        if args.mode == "api":
            print(
                call_real_llm_api(
                    request_id,
                    role,
                    prompt,
                    args.api_key,
                    args.model,
                    args.api_url,
                ),
                flush=True,
            )
        else:
            print(demo_model_response(request_id, role, prompt), flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
