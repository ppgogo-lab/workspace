# MM Default Stopped On Startup

## Plan

- Make the product market-making `enabled` flag default to `false` in code.
- Update shipped product configs so demo backends do not quote immediately after startup.
- Keep persistence enabled because it only controls the SQLite writer, not strategy activation.
- Rebuild and restart the WSL backend, then verify it listens on the expected gRPC port.

## Implementation

- Changed `MMParamsConfig::enabled` and `AtomicMMParams::enabled` defaults to `false`.
- Changed YAML parsing so omitted product `params.enabled` fields default to `false`.
- Set product-level `params.enabled: false` in the shipped demo/FEMAS configs.
- Documented in `AGENTS.md` that backend startup leaves MM stopped and the GUI `Start MM` button must start it explicitly.

## Test Result

- `git diff --check` passed; Git only reported expected Windows line-ending warnings.
- Rebuilt the WSL backend with `cmake --build build-wsl --target optionmm -j4`.
- Restarted the backend with `scripts/run_sim_demo.sh`; PID `397` is running.
- Verified startup log contains `gRPC server listening on 0.0.0.0:50061`.
- Verified `strategy_params` persisted `enabled = 0` for product indexes `0` and `1`.
