## Summary

<!-- What does this PR do and why. A couple sentences is fine. -->

## Component(s) changed

<!-- firmware / pi-server / relay / docs / tests / other -->

## Testing done

- [ ] `pi-server` unit tests (`cd pi-server && python -m pytest -q`)
- [ ] `relay` unit tests (`cd relay && python -m pytest -q`)
- [ ] Integration tests (`python -m pytest tests/integration -q`, from repo root with pi-server's venv active)
- [ ] Firmware host tests (`cd firmware/test && cmake -B build && cmake --build build && ctest --test-dir build --output-on-failure`)
- [ ] N/A — explain why below

<!-- Only check the suites relevant to what you touched; note anything you couldn't run and why. -->

## Related issue

<!-- Closes #... / relates to #... / none -->
