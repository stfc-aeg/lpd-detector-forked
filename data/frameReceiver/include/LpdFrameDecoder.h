/*
 * LpdEmulatorFrameDecoder.h
 *
 *  Created on: Jan 16, 2017
 *      Author: Tim Nicholls, STFC Application Engineering Group
 */

#ifndef INCLUDE_LPDFRAMEDECODER_H_
#define INCLUDE_LPDFRAMEDECODER_H_

#include "FrameDecoderUDP.h"
#include "LpdDefinitions.h"
#include <iostream>
#include <map>
#include <stdint.h>
#include <time.h>

#define ILLEGAL_FEM_IDX -1

const std::string default_fem_port_map = "61649:0";

namespace FrameReceiver
{
  typedef struct LpdDecoderFemMapEntry
  {
    int fem_idx_;
    unsigned int buf_idx_;

    LpdDecoderFemMapEntry(int fem_idx=ILLEGAL_FEM_IDX, int buf_idx=ILLEGAL_FEM_IDX) :
      fem_idx_(fem_idx),
      buf_idx_(buf_idx)
    {};
  } LpdDecoderFemMapEntry;

  typedef std::map<int, LpdDecoderFemMapEntry> LpdDecoderFemMap;

  class LpdFrameDecoder : public FrameDecoderUDP
  {
  public:

    LpdFrameDecoder();
    ~LpdFrameDecoder();

    void init(LoggerPtr& logger, OdinData::IpcMessage& config_msg);
    void request_configuration(const std::string param_prefix, OdinData::IpcMessage& config_reply);

    const size_t get_frame_buffer_size(void) const;
    const size_t get_frame_header_size(void) const;

    inline const bool requires_header_peek(void) const
    {
//      return true;
    	return false;
    };

    inline const bool trailer_mode(void) const
        {
          return true;
//          return false;
        };

    const size_t get_packet_header_size(void) const;
    void process_packet_header (size_t bytes_received, int port,
        struct sockaddr_in* from_addr);

    void* get_next_payload_buffer(void) const;
    size_t get_next_payload_size(void) const;
    FrameDecoder::FrameReceiveState process_packet (size_t bytes_received, int port, struct sockaddr_in* from_addr);

    void monitor_buffers(void);
    void get_status(const std::string param_prefix, OdinData::IpcMessage& status_msg);

    void* get_packet_header_buffer(void);

    uint32_t get_frame_number(uint8_t* &trlr_ptr) const;
    uint32_t get_packet_number(uint8_t* &trlr_ptr) const;
    bool get_start_of_frame_marker(uint8_t* &trlr_ptr) const;
    bool get_end_of_frame_marker(uint8_t* &trlr_ptr) const;


  private:

    void initialise_frame_header(Lpd::FrameHeader* header_ptr);
    unsigned int elapsed_ms(struct timespec& start, struct timespec& end);
    std::size_t parse_fem_port_map(const std::string fem_port_map_str);
    Lpd::AsicCounterBitDepth parse_bit_depth(const std::string bit_depth_str);

    Lpd::AsicCounterBitDepth asic_counter_bit_depth_;
    std::string fem_port_map_str_;
    LpdDecoderFemMap fem_port_map_;
    boost::shared_ptr<void> current_packet_trailer_;
    boost::shared_ptr<void> dropped_frame_buffer_;
    boost::shared_ptr<void> ignored_packet_buffer_;

    int current_frame_seen_;
    int current_frame_buffer_id_;
    void* current_frame_buffer_;
    Lpd::FrameHeader* current_frame_header_;
    LpdDecoderFemMapEntry current_packet_fem_map_;
    std::size_t num_active_fems_;

    bool dropping_frame_data_;
    uint32_t packets_ignored_;
    uint32_t packets_lost_;
    uint32_t fem_packets_lost_[Lpd::max_num_fems];

    static const std::string asic_bit_depth_str_[Lpd::num_bit_depths];

    static const std::string CONFIG_FEM_PORT_MAP;
    static const std::string CONFIG_BITDEPTH;


  };

} // namespace FrameReceiver

#endif /* INCLUDE_LPDFRAMEDECODER_H_ */
