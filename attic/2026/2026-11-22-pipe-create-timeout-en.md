# Give the pipe server a longer forward timeout

## Problem

bash printed `start_pipeline: pgrp pipe: Out of memory` on the first command. The kernel maps every `pipe()` failure to ENOMEM. The failing call is the `PIPE_CREATE` forward: under load the pipe server sometimes answers later than the router's 1000 ms timeout, so the caller gives up even though the pipe server created the pipe.

## Fix

Forward `PIPE_CREATE` with `router_forward_timeout(SVC_PIPE, ..., 5000)`. The pipe server answers before the longer deadline, so `pipe()` succeeds. A retry would create a second pipe and leak the first; the longer wait avoids that.

## Verification

The stress run reports no `start_pipeline` or `PIPE_CREATE` failure and passes.
