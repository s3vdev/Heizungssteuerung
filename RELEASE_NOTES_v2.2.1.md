# Release Notes v2.2.1

## Fixes
- Fixed heating relay control by moving the default heating relay pin from GPIO23 to GPIO21 (GPIO23 was unreliable on some boards).
- Fixed boot restore logic: if heating is ON after reboot, the pump is now forced ON and the GPIO state is applied reliably.
- Fixed dashboard crashes caused by incomplete `/api/status` payload (schedules/time fields). The firmware JSON buffer was increased and the frontend now applies safe defaults.

## Improvements
- Added advanced relay configuration in the dashboard (polarity + OFF mode).
- Reduced USB upload baud rate to 460800 for more reliable flashing.


