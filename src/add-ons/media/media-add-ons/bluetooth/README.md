# Bluetooth audio output

This media add-on implements an A2DP source for a classic Bluetooth headphone
or speaker. The Bluetooth server discovers the Audio Sink service after
authentication and encryption, then sends its address to the add-on. AVDTP
discovery, configuration, opening, and starting run on a dedicated thread.

Select **Bluetooth audio** as the audio output in Media preferences. The normal
system mixer supplies 48 kHz stereo float PCM; Media Kit handles conversion from
application formats. The node exposes master volume, mute, codec preference,
and one bitrate selector in its parameter web. Volume and mute apply locally to
PCM. Codec and bitrate changes take effect on the next connection. Settings are saved in
`~/config/settings/Media/bluetooth_audio_settings` on node shutdown.

Automatic selection prefers LDAC, then AAC, then SBC. Negotiation accepts only
48 kHz stereo configurations supported by both the peer and the encoder:

* LDAC uses the bundled Sony/AOSP encoder. The unified selector provides 128,
  256, 330, 660, and 990 kb/s targets; frame-length granularity produces 129
  and 255 kb/s for the first two targets at 48 kHz.
  Aggregated encoder output is split into complete LDAC frames for RTP packets.
* AAC uses FFmpeg's AAC-LC encoder and LATM muxer. The remote endpoint must
  support VBR. The target rate is capped at 128 kb/s and at the peer's advertised
  limit. RTP carries AudioMuxElement without the LOAS header; larger elements
  are fragmented with a common timestamp and a marker on the final packet.
* SBC uses FFmpeg with 16 blocks, 8 subbands, loudness allocation, joint stereo,
  and derives a bitpool no greater than 53 or the peer's maximum from the same
  bitrate selector.

There is one output and one connected audio device at a time. This implements
audio playback; microphone/headset telephony, receiving audio as an A2DP sink,
AVRCP remote controls/absolute volume, and LE Audio are separate profiles.
The node reports an estimated 100 ms latency. Headphone-specific delay reporting
and controller congestion adaptation are not implemented. Validate sustained
playback with the intended controller/headphone, especially at LDAC high quality.

The radio worker uses a bounded queue and socket timeouts. The media event thread
never waits for Bluetooth I/O. Disconnects close both AVDTP channels. Errors include
the failed connection stage; remote rejections include the AVDTP signal and code.

## Building and testing

From a configured build directory with the FFmpeg build feature:

```
jam -q -j4 bluetooth_audio.media_addon bluetooth_server btCoreData l2cap
jam -q -j4 bluetooth_audio_protocol_test bluetooth_audio_test bluetooth_audio_media_test
```

Run the three test programs on Haiku. The protocol test also builds on a host
with a C++11 compiler from `ProtocolTest.cpp` and `A2dpProtocol.cpp`. The audio
test encodes a tone in uneven input chunks, decodes AAC/SBC, checks LDAC frames,
and verifies actual RTP datagrams through a socket pair. The media test registers
an application-owned physical output with the media server and verifies the
input format and mixer parameter round trips, without changing the default output.

The regular image includes this add-on when FFmpeg is enabled. Install/rebuild
the matching kernel Bluetooth modules and Bluetooth server as well: the media
transport uses the new `SO_L2CAP_OUTGOING_MTU` socket option. The build does not
install into or restart an already running Haiku system automatically.

Protocol/codec references:

* Bluetooth SIG A2DP and AVDTP specifications:
  https://www.bluetooth.com/specifications/specs/
* FFmpeg 6.1 AAC/SBC codec and LATM muxer implementations:
  https://github.com/FFmpeg/FFmpeg/tree/n6.1.5
* Sony/AOSP LDAC encoder provenance and license: `src/libs/ldac/README.haiku`.
