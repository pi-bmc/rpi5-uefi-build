/** @file

  RPI_FAN_PROTOCOL - in-firmware control surface for the Raspberry Pi 5
  active cooler, produced by ActiveCoolerDxe.

  Consumers (the Setup page, RpiRedfishSyncDxe's thermal telemetry, shell
  tools) read the SoC temperature and commanded fan state through GetInfo,
  and can pin the fan to a level with SetOverride. An override is volatile
  and BMC/operator-owned: it wins over whatever the FanPolicy variable
  says until ClearOverride, and does not survive a reset. Persistent
  policy belongs in the FanPolicy variable (see Guid/RpiFanPolicy.h).

  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#ifndef RPI_FAN_H_
#define RPI_FAN_H_

#define RPI_FAN_PROTOCOL_GUID \
  { 0x9d8a2c6e, 0x41b7, 0x4c15, { 0x9e, 0x02, 0x7a, 0x33, 0xd1, 0x5f, 0x28, 0x64 } }

typedef struct _RPI_FAN_PROTOCOL RPI_FAN_PROTOCOL;

typedef struct {
  ///
  /// Last SoC temperature reading, in milli-degrees Celsius. Only
  /// meaningful when TemperatureValid is TRUE (the AVS monitor's validity
  /// bits can be clear early after reset).
  ///
  INT32      TemperatureMilliCelsius;
  BOOLEAN    TemperatureValid;
  ///
  /// Currently commanded cooling level, 0 (off) .. MaxLevel.
  ///
  UINT8      Level;
  UINT8      MaxLevel;
  ///
  /// Commanded PWM duty on the DTB's 0..255 scale (the pwm-fan
  /// cooling-levels convention; 255 = full speed).
  ///
  UINT32     Duty255;
  ///
  /// TRUE while a SetOverride level is pinning the fan.
  ///
  BOOLEAN    OverrideActive;
} RPI_FAN_INFO;

/**
  Read the current fan state and a fresh temperature sample.

  @param[in]  This  Protocol instance.
  @param[out] Info  Receives the fan state.

  @retval EFI_SUCCESS            Info was filled in.
  @retval EFI_INVALID_PARAMETER  Info is NULL.
  @retval EFI_NOT_READY          The RP1 (and so the PWM) is not connected
                                 yet; only the temperature fields are valid.
**/
typedef
EFI_STATUS
(EFIAPI *RPI_FAN_GET_INFO)(
  IN  RPI_FAN_PROTOCOL  *This,
  OUT RPI_FAN_INFO      *Info
  );

/**
  Pin the fan to a fixed cooling level, overriding the FanPolicy variable
  and the automatic loop until ClearOverride.

  Takes effect immediately when the PWM is up, otherwise as soon as it is.

  @param[in] This   Protocol instance.
  @param[in] Level  Cooling level to pin, 0 .. MaxLevel.

  @retval EFI_SUCCESS            Override staged (and applied if possible).
  @retval EFI_INVALID_PARAMETER  Level is out of range.
**/
typedef
EFI_STATUS
(EFIAPI *RPI_FAN_SET_OVERRIDE)(
  IN RPI_FAN_PROTOCOL  *This,
  IN UINT8             Level
  );

/**
  Drop any pinned level and return the fan to policy control.

  @param[in] This  Protocol instance.

  @retval EFI_SUCCESS  Always; a no-op when no override was active.
**/
typedef
EFI_STATUS
(EFIAPI *RPI_FAN_CLEAR_OVERRIDE)(
  IN RPI_FAN_PROTOCOL  *This
  );

struct _RPI_FAN_PROTOCOL {
  RPI_FAN_GET_INFO          GetInfo;
  RPI_FAN_SET_OVERRIDE      SetOverride;
  RPI_FAN_CLEAR_OVERRIDE    ClearOverride;
};

extern EFI_GUID  gRpiFanProtocolGuid;

#endif // RPI_FAN_H_
