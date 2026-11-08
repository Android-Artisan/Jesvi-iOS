# CoreMotion gyro probe

Build from the repository root, then run on the connected iOS phone:

```sh
xnu-deps/host/bin/xnu-clang -arch arm64 -isysroot /Users/jesvi/theos/sdks/iPhoneOS16.5.sdk -O1 -framework Foundation -framework CoreMotion -o /tmp/gyro-probe tools/sensors/gyro-probe.m
scp -O /tmp/gyro-probe s20:/var/tmp/gyro-probe
ssh s20 /var/tmp/gyro-probe
```

The probe runs for about 20 seconds plus framework startup time. It reports
real samples received through CMMotionManager, peak rotation rate, accumulated
angles and whether fused device-motion samples arrive. Rotate about one axis,
hold still, then return. A 90-degree turn corresponds to about 1.57 radians;
returning approximately cancels the signed angle. Public gyro rates are rad/s;
Apple's HID gyro events use deg/s and CoreMotion performs that conversion.
This probe does not change sensor settings beyond starting/stopping its own
CoreMotion subscriptions, nor does it override the bridge's input.
