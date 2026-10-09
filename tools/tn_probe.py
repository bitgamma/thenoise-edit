#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""M0.5 acceptance: what the real Lemonade/thenoise server actually does.

Nothing here asserts anything about *image content* - a diffusion model's output is not a
golden file. What is asserted is the contract the client is written against: what discovery
says, what the endpoints accept, what a refusal looks like, and what comes back for a size we
asked for. Timings are printed for `docs/probes/` and never asserted, because the box is
shared and a loaded model changes everything.

    THENOISE_URL=... THENOISE_API_KEY=*** python3 tools/tn_probe.py

Skips loudly (exit 0, "SKIP:" on stdout) without that environment, so the default ctest run
stays hermetic. `--max-images` caps how many requests it will make: the point is to learn the
contract, not to burn the box.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import sys
import time
import urllib.error
import urllib.request

DEFAULT_MODEL = "Flux-2-Klein-4B-TheNoise"
SNAP = 16  # the engine snaps a requested size to this multiple; measured, not assumed


class Budget:
    def __init__(self, max_images: int) -> None:
        self.remaining = max_images

    def take(self, what: str) -> None:
        self.remaining -= 1
        if self.remaining < 0:
            raise Skip(f"image budget exhausted before {what}")


class Skip(Exception):
    pass


class Client:
    def __init__(self, base_url: str, api_key: str, timeout: float) -> None:
        self.base = base_url.rstrip("/")
        self.key = api_key
        self.timeout = timeout

    # A refusal is data here: the client has to branch on the shape of an error body, so the
    # status and the body come back instead of an exception.
    def _request(self, path: str, body: bytes, content_type: str, method: str = "POST"):
        request = urllib.request.Request(
            self.base + path,
            data=body,
            headers={"Authorization": f"Bearer {self.key}",
                     "Content-Type": content_type},
            method=method,
        )
        started = time.perf_counter()
        try:
            with urllib.request.urlopen(request, timeout=self.timeout) as response:
                return response.status, response.read(), time.perf_counter() - started
        except urllib.error.HTTPError as error:
            return error.code, error.read(), time.perf_counter() - started

    def _json(self, raw):
        try:
            return json.loads(raw)
        except (json.JSONDecodeError, TypeError):
            return raw.decode(errors="replace") if isinstance(raw, bytes) else raw

    def get(self, path: str):
        status, raw, took = self._request(path, b"", "application/json", method="GET")
        return status, self._json(raw), took

    def post_json(self, path: str, payload: dict):
        status, raw, took = self._request(path, json.dumps(payload).encode(), "application/json")
        return status, self._json(raw), took

    def post_multipart(self, path: str, fields: dict, image: bytes):
        import uuid

        boundary = uuid.uuid4().hex
        body = bytearray()
        for name, value in fields.items():
            body += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"\r\n\r\n"
                     f"{value}\r\n").encode()
        body += (f"--{boundary}\r\nContent-Disposition: form-data; name=\"image\"; "
                 f"filename=\"image.png\"\r\nContent-Type: image/png\r\n\r\n").encode()
        body += image + b"\r\n"
        body += f"--{boundary}--\r\n".encode()
        status, raw, took = self._request(path, bytes(body), f"multipart/form-data; boundary={boundary}")
        return status, self._json(raw), took



def png_dims(data: bytes) -> tuple[int, int]:
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise AssertionError(f"response is not a PNG (starts {data[:8]!r})")
    return int.from_bytes(data[16:20], "big"), int.from_bytes(data[20:24], "big")


def image_of(reply: dict) -> bytes:
    return base64.b64decode(reply["data"][0]["b64_json"])


def check(label: str, condition: bool, detail: str, fails: list[str]) -> None:
    print(f"  {'ok  ' if condition else 'FAIL'} {label}{'' if condition else ': ' + detail}")
    if not condition:
        fails.append(f"{label}: {detail}")


def probe_discovery(client: Client, model: str, fails: list[str]) -> dict:
    print("GET /health")
    status, health, took = client.get("/health")
    check("health answers", status == 200 and health.get("status") == "ok", f"status={status}", fails)
    print(f"       server {health.get('version')}, model_loaded={health.get('model_loaded')}, "
          f"one model per class={health.get('max_models', {}).get('image')} ({took * 1000:.0f} ms)")

    print("GET /models")
    status, whole, took = client.get("/models?show_all=true")
    print(f"       show_all=true: http={status}, {len(whole.get('data', []))} entries "
          f"({took * 1000:.0f} ms)")
    status, listing, took = client.get("/models")
    check("models answers", status == 200 and isinstance(listing.get("data"), list),
          f"status={status}", fails)
    candidates = [m for m in listing.get("data", [])
                  if "image" in m.get("labels", []) and m.get("recipe") == "thenoise"]
    print(f"       {len(listing.get('data', []))} models, {len(candidates)} thenoise image models "
          f"({took * 1000:.0f} ms)")
    for entry in sorted(candidates, key=lambda m: m["id"]):
        print(f"         {entry['id']:30s} labels={','.join(entry['labels']):16s} "
              f"downloaded={str(entry['downloaded']):5s} size={entry.get('size', 0):5.1f} GB "
              f"defaults={entry.get('image_defaults')}")
    chosen = next((m for m in listing.get("data", []) if m["id"] == model), None)
    check(f"'{model}' is listed", chosen is not None, "not in /models", fails)
    if chosen:
        check("  it is a thenoise recipe", chosen.get("recipe") == "thenoise",
              f"recipe={chosen.get('recipe')}", fails)
        check("  it is downloaded", chosen.get("downloaded") is True, "downloaded=false", fails)
        check("  labels carry a capability set", bool(chosen.get("labels")), "no labels", fails)
    return chosen or {}


def probe_generation(client: Client, model: str, defaults: dict, sizes: list[str],
                     budget: Budget, fails: list[str]) -> bytes:
    print(f"POST /images/generations  (model {model}, steps {defaults.get('steps', 4)})")
    reference = b""
    for size in sizes:
        budget.take(f"generation at {size}")
        payload = {"model": model, "prompt": "a smooth grey gradient, soft studio light",
                   "size": size, "steps": defaults.get("steps", 4),
                   "cfg_scale": defaults.get("cfg_scale", 1.0),
                   # A fresh seed every time: an identical request can be answered from the
                   # server's own cache, which would be measured as inference.
                   "seed": int(time.time()) % 1_000_000 + len(size),
                   "response_format": "b64_json"}
        status, reply, took = client.post_json("/images/generations", payload)
        if status != 200:
            check(f"generate {size}", False, f"http={status} {str(reply)[:160]}", fails)
            continue
        image = image_of(reply)
        width, height = png_dims(image)
        want_w, want_h = (int(part) for part in size.lower().split("x"))
        check(f"generate {size}", width % SNAP == 0 and height % SNAP == 0,
              f"returned {width}x{height}, not a multiple of {SNAP}", fails)
        note = "" if (width, height) == (want_w, want_h) else f" (asked {want_w}x{want_h})"
        print(f"       {size:10s} {took:7.1f}s -> {width}x{height}{note}, "
              f"{len(image) / 1024:.0f} KiB")
        if size.lower() == "512x512":
            reference = image
    return reference


def probe_edit(client: Client, model: str, defaults: dict, reference: bytes, budget: Budget,
               fails: list[str]) -> None:
    print("POST /images/edits")
    fields = {"model": model, "prompt": "make the gradient warm, keep it a smooth gradient",
              "size": "512x512", "steps": defaults.get("steps", 4),
              "cfg_scale": defaults.get("cfg_scale", 1.0), "seed": 77,
              "response_format": "b64_json"}
    budget.take("edit at 512x512")
    status, reply, took = client.post_multipart("/images/edits", fields, reference)
    if status != 200:
        check("edit with explicit size", False, f"http={status} {str(reply)[:200]}", fails)
        return
    width, height = png_dims(image_of(reply))
    check("edit honours an explicit size", (width, height) == (512, 512),
          f"returned {width}x{height}", fails)
    print(f"       edit 512x512 {took:6.1f}s -> {width}x{height}")

    # The region compiler always sends a size precisely because the answer without one is not
    # the input's size. Re-measured on every run because it is an engine behaviour, not ours.
    budget.take("edit without a size")
    omit = {k: v for k, v in fields.items() if k != "size"}
    status, reply, took = client.post_multipart("/images/edits", omit, reference)
    if status == 200:
        print(f"       edit, size omitted {took:6.1f}s -> {png_dims(image_of(reply))}")
    else:
        print(f"       edit, size omitted -> http={status}")

    # A step must belong to the request that produced it: two identical payloads, both warm,
    # give the same bytes. Whether that is the sampler or the server's cache answering, the
    # client's own replay cache stays valid either way.
    budget.take("replay")
    status_a, reply_a, _ = client.post_json(
        "/images/generations", {"model": model, "prompt": "repro", "size": "64x64",
                                "steps": 1, "seed": 4242, "response_format": "b64_json"})
    status_b, reply_b, _ = client.post_json(
        "/images/generations", {"model": model, "prompt": "repro", "size": "64x64",
                                "steps": 1, "seed": 4242, "response_format": "b64_json"})
    if status_a == 200 and status_b == 200:
        check("same request twice is byte-identical", image_of(reply_a) == image_of(reply_b),
              "same prompt and seed returned different bytes", fails)


def probe_failures(client: Client, model: str, budget: Budget, fails: list[str]) -> None:
    print("failure shapes the client has to branch on")
    budget.take("unknown model")
    status, reply, _ = client.post_json("/images/generations",
                                        {"model": "NoSuchModel-TheNoise", "prompt": "x",
                                         "size": "64x64", "steps": 1,
                                         "response_format": "b64_json"})
    message = reply.get("error", {}) if isinstance(reply, dict) else {}
    check("unknown model is 404 with error.code",
          status == 404 and message.get("code") == "model_not_found",
          f"http={status} body={str(reply)[:160]}", fails)

    budget.take("missing prompt")
    status, reply, _ = client.post_json("/images/generations",
                                        {"model": model, "size": "64x64", "steps": 1,
                                         "response_format": "b64_json"})
    message = reply.get("error", {}) if isinstance(reply, dict) else {}
    check("missing prompt is 400 with error.message",
          status == 400 and bool(message.get("message")),
          f"http={status} body={str(reply)[:160]}", fails)

    # A field the server does not understand is not refused: it is ignored. Everything the
    # request builder emits therefore has to be validated on our side, because a typo in a
    # field name would silently mean "the default" rather than an error.
    budget.take("bad seed type")
    status, reply, _ = client.post_json("/images/generations",
                                        {"model": model, "prompt": "x", "size": "64x64",
                                         "steps": 1, "seed": "not-a-number",
                                         "response_format": "b64_json"})
    print(f"       a malformed 'seed' -> http={status} "
          f"({'accepted and ignored' if status == 200 else str(reply)[:120]})")


def probe_upscale(client: Client, budget: Budget, fails: list[str], reference: bytes) -> None:
    if not reference:
        print("POST /images/upscale skipped: no reference image")
        return
    print("POST /images/upscale")
    budget.take("upscale")
    status, reply, took = client.post_json(
        "/images/upscale", {"model": "RealESRGAN-4xplus-TheNoise",
                            "image": base64.b64encode(reference).decode()})
    if status != 200:
        print(f"       http={status}: {str(reply)[:160]}")
        return
    width, height = png_dims(image_of(reply))
    check("upscale returns a bigger image", (width, height) >= (512, 512),
          f"512x512 in, {width}x{height} out", fails)
    print(f"       512x512 -> {width}x{height} in {took:.1f}s")


def run(args: argparse.Namespace) -> int:
    client = Client(os.environ["THENOISE_URL"], os.environ["THENOISE_API_KEY"], args.timeout)
    budget = Budget(args.max_images)
    fails: list[str] = []
    print(f"server: {os.environ['THENOISE_URL']}")
    defaults = probe_discovery(client, args.model, fails)
    reference = probe_generation(client, args.model, defaults, args.sizes, budget, fails)
    probe_edit(client, args.model, defaults, reference, budget, fails)
    probe_failures(client, args.model, budget, fails)
    if args.upscale:
        probe_upscale(client, budget, fails, reference)
    for failure in fails:
        print("FAIL", failure)
    return 1 if fails else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--sizes", nargs="*", default=["512x512", "1024x1024"])
    parser.add_argument("--timeout", type=float, default=900.0)
    parser.add_argument("--max-images", type=int, default=10)
    parser.add_argument("--upscale", action="store_true")
    args = parser.parse_args()

    if not os.environ.get("THENOISE_URL") or not os.environ.get("THENOISE_API_KEY"):
        print("SKIP: THENOISE_URL and THENOISE_API_KEY are not set, so the live tier cannot run")
        return 0
    try:
        return run(args)
    except Skip as skip:
        print(f"SKIP: {skip}")
        return 0
    except urllib.error.HTTPError as error:
        print(f"FAIL http={error.code}: {error.read().decode(errors='replace')[:400]}")
        return 1
    except (urllib.error.URLError, TimeoutError) as error:
        print(f"FAIL cannot reach {os.environ.get('THENOISE_URL')}: {error}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
