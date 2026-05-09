param(
    [string]$ComPort = "COM3",
    [string]$Environment = "sonoff_s31",
    [int]$Baud = 460800
)

if ($Environment -eq "sonoff_s31_bootstrap") {
    py -m esptool --port $ComPort --baud $Baud --no-stub write-flash --flash-size detect --no-compress 0x00000 "C:\dev\rebooter-firmware\.pio\build\sonoff_s31_bootstrap\firmware.bin"
} else {
    pio run -e $Environment -t upload --upload-port $ComPort
}
