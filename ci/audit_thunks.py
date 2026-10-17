#!/usr/bin/env python3
"""Audit w64_thunks.asm thunk arities against the guest ABI (psb_guest_api.h).
Ground truth = guest-side declarations."""
import re

root = "/home/z/my-project/psbet_repo"
asm = open(f"{root}/src/platform/w64_thunks.asm").read()
hdr = open(f"{root}/include/psbet/psb_guest_api.h").read()

uses = re.findall(r"thunk(\d)\s+(psbT\w+),\s*(psbX\w+)", asm)

def decl_params(name):
    m = re.search(re.escape(name) + r"\s*\(", hdr)
    if not m:
        return None
    i = m.end()
    depth = 1
    while depth:
        c = hdr[i]
        if c == "(": depth += 1
        elif c == ")": depth -= 1
        i += 1
    body = hdr[m.end():i-1].strip()
    if body in ("", "void"):
        return 0
    depth, n, inparen = 0, 1, 0
    for c in body:
        if c == "(" : inparen += 1
        elif c == ")": inparen -= 1
        elif c == "," and inparen == 0: n += 1
    return n

def guest_name(psbx):
    m = re.match(r"psbXSys(.+)", psbx)
    if m: return "sceKernel" + m.group(1)
    m = re.match(r"psbXGnm(.+)", psbx)
    if m: return "sceGnm" + m.group(1)
    m = re.match(r"psbX(.+)", psbx)
    if m: return m.group(1)
    return None

bad = 0
for arity, thunk, target in uses:
    arity = int(arity)
    g = guest_name(target)
    n = decl_params(g) if g else None
    if n is None:
        print(f"?  thunk{arity} {thunk:40s} -> {target:38s} guest decl not found for '{g}'")
        continue
    ok = "OK " if n == arity else "!! "
    if n != arity: bad += 1
    print(f"{ok}thunk{arity} {thunk:40s} -> {g:42s} guest arity = {n}")

print(f"\n{bad} mismatches out of {len(uses)} thunk uses")
