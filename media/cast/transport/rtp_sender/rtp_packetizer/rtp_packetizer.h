// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_CAST_TRANSPORT_RTP_SENDER_RTP_PACKETIZER_RTP_PACKETIZER_H_
#define MEDIA_CAST_TRANSPORT_RTP_SENDER_RTP_PACKETIZER_RTP_PACKETIZER_H_

#include <cmath>
#include <list>
#include <map>

#include "base/time/time.h"
#include "media/cast/transport/rtp_sender/packet_storage/packet_storage.h"

namespace media {
namespace cast {
namespace transport {

class PacedSender;

struct RtpPacketizerConfig {
  RtpPacketizerConfig();
  ~RtpPacketizerConfig();

  // General.
  bool audio;
  int payload_type;
  uint16 max_payload_length;
  uint16 sequence_number;
  uint32 rtp_timestamp;
  int frequency;

  // SSRC.
  unsigned int ssrc;

  // Video.
  VideoCodec video_codec;

  // Audio.
  uint8 channels;
  AudioCodec audio_codec;
};

// This object is only called from the main cast thread.
// This class break encoded audio and video frames into packets and add an RTP
// header to each packet.
class RtpPacketizer {
 public:
  RtpPacketizer(PacedSender* const transport,
                PacketStorage* packet_storage,
                RtpPacketizerConfig rtp_packetizer_config);
  ~RtpPacketizer();

  // The video_frame objects ownership is handled by the main cast thread.
  void IncomingEncodedVideoFrame(const EncodedVideoFrame* video_frame,
                                 const base::TimeTicks& capture_time);

  // The audio_frame objects ownership is handled by the main cast thread.
  void IncomingEncodedAudioFrame(const EncodedAudioFrame* audio_frame,
                                 const base::TimeTicks& recorded_time);

  bool LastSentTimestamp(base::TimeTicks* time_sent,
                         uint32* rtp_timestamp) const;

  // Return the next sequence number, and increment by one. Enables unique
  // incremental sequence numbers for every packet (including retransmissions).
  uint16 NextSequenceNumber();

  int send_packets_count() { return send_packets_count_; }

  size_t send_octet_count() { return send_octet_count_; }

 private:
  void Cast(bool is_key, uint32 frame_id, uint32 reference_frame_id,
            uint32 timestamp, const std::string& data);

  void BuildCommonRTPheader(Packet* packet, bool marker_bit,
      uint32 time_stamp);

  RtpPacketizerConfig config_;
  PacedSender* const transport_;  // Not owned by this class.
  PacketStorage* packet_storage_;

  base::TimeTicks time_last_sent_rtp_timestamp_;
  uint16 sequence_number_;
  uint32 rtp_timestamp_;
  uint16 packet_id_;

  int send_packets_count_;
  size_t send_octet_count_;
};

}  // namespace transport
}  // namespace cast
}  // namespace media

#endif  // MEDIA_CAST_TRANSPORT_RTP_SENDER_RTP_PACKETIZER_RTP_PACKETIZER_H_
