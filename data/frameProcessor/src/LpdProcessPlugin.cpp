/*
 * LpdProcessPlugin.cpp
 *
 *  Created on: 6 Jun 2016
 *      Author: gnx91527
 */

#include <LpdProcessPlugin.h>

namespace FrameProcessor
{

  const std::string LpdProcessPlugin::CONFIG_DROPPED_PACKETS = "packets_lost";
  const std::string LpdProcessPlugin::CONFIG_IMAGE_WIDTH = "width";
  const std::string LpdProcessPlugin::CONFIG_IMAGE_HEIGHT = "height";
  const std::string LpdProcessPlugin::CONFIG_NUM_IMAGES = "num_images";

  /**
   * The constructor sets up logging used within the class.
   */
  LpdProcessPlugin::LpdProcessPlugin() :
    image_width_(256),
    image_height_(256),
    image_pixels_(image_width_ * image_height_),
    num_images_(20),
    packets_lost_(0),
    image_counter_(0)
  {
    // Setup logging for the class
    logger_ = Logger::getLogger("FW.LpdProcessPlugin");
    logger_->setLevel(Level::getAll());
    LOG4CXX_TRACE(logger_, "LpdProcessPlugin constructor.");
  }

  /**
   * Destructor.
   */
  LpdProcessPlugin::~LpdProcessPlugin()
  {
    LOG4CXX_TRACE(logger_, "LpdProcessPlugin destructor.");
  }

  /**
   * Configure the Lpd plugin.  This receives an IpcMessage which should be processed
   * to configure the plugin, and any response can be added to the reply IpcMessage.  This
   * plugin supports the following configuration parameters:
   * \param[in] config - Reference to the configuration IpcMessage object.
   * \param[out] reply - Reference to the reply IpcMessage object.
   */
  void LpdProcessPlugin::configure(OdinData::IpcMessage& config, OdinData::IpcMessage& reply)
  {
    if (config.has_param(LpdProcessPlugin::CONFIG_DROPPED_PACKETS))
    {
      packets_lost_ = config.get_param<int>(LpdProcessPlugin::CONFIG_DROPPED_PACKETS);
    }

    if (config.has_param(LpdProcessPlugin::CONFIG_IMAGE_WIDTH))
    {
      image_width_ = config.get_param<int>(LpdProcessPlugin::CONFIG_IMAGE_WIDTH);
    }

    if (config.has_param(LpdProcessPlugin::CONFIG_IMAGE_HEIGHT))
    {
      image_height_ = config.get_param<int>(LpdProcessPlugin::CONFIG_IMAGE_HEIGHT);
    }

    if (config.has_param(LpdProcessPlugin::CONFIG_NUM_IMAGES))
    {
      num_images_ = config.get_param<int>(LpdProcessPlugin::CONFIG_NUM_IMAGES);
    }

    image_pixels_ = image_width_ * image_height_;

  }

  /**
   * Collate status information for the plugin.  The status is added to the status IpcMessage object.
   *
   * \param[out] status - Reference to an IpcMessage value to store the status.
   */
  void LpdProcessPlugin::status(OdinData::IpcMessage& status)
  {
    // Record the plugin's status items
    LOG4CXX_DEBUG(logger_, "Status requested for Lpd plugin");
    status.set_param(get_name() + "/packets_lost", packets_lost_);
  }

  /**
   * Process and report lost UDP packets for the frame
   *
   * \param[in] frame - Pointer to a Frame object.
   */
  void LpdProcessPlugin::process_lost_packets(boost::shared_ptr<Frame> frame)
  {
    const Lpd::FrameHeader* hdr_ptr = static_cast<const Lpd::FrameHeader*>(frame->get_data());
    LOG4CXX_DEBUG(logger_, "Processing lost packets for frame " << hdr_ptr->frame_number);
    LOG4CXX_DEBUG(logger_, "Packets received: " << hdr_ptr->total_packets_received
        << " out of a maximum "
        << Lpd::num_packets * hdr_ptr->num_active_fems);
    if (hdr_ptr->total_packets_received < Lpd::num_packets * hdr_ptr->num_active_fems){
      int packets_lost = (Lpd::num_packets * hdr_ptr->num_active_fems) - hdr_ptr->total_packets_received;
      LOG4CXX_ERROR(logger_, "Frame number " << hdr_ptr->frame_number << " has dropped " << packets_lost << " packets");
      packets_lost_ += packets_lost;
      LOG4CXX_ERROR(logger_, "Total packets lost since startup " << packets_lost_);
    }
  }



  /**
   * Perform processing on the frame.
   *
   * \param[in] frame - Pointer to a Frame object.
   */
  void LpdProcessPlugin::process_frame(boost::shared_ptr<Frame> frame)
  {
    LOG4CXX_TRACE(logger_, "Reordering frame.");
    LOG4CXX_TRACE(logger_, "Frame size: " << frame->get_data_size());

    this->process_lost_packets(frame);

    const Lpd::FrameHeader* hdr_ptr =
        static_cast<const Lpd::FrameHeader*>(frame->get_data());

    LOG4CXX_TRACE(logger_, "Raw frame number: " << hdr_ptr->frame_number);
    LOG4CXX_TRACE(logger_, "Frame state: " << hdr_ptr->frame_state);
    LOG4CXX_TRACE(logger_, "Packets received: " << hdr_ptr->total_packets_received
        << " SOF markers: "<< (int)hdr_ptr->total_sof_marker_count
        << " EOF markers: "<< (int)hdr_ptr->total_eof_marker_count);

    // Loop over the active FEM list to determine the maximum active FEM index

    unsigned int max_active_fem_idx = 0;
    {
      std::stringstream msg;
      msg << "Number of active FEMs: " << static_cast<int>(hdr_ptr->num_active_fems) << " ids:";
      for (uint8_t idx = 0; idx < hdr_ptr->num_active_fems; idx++)
      {
        if (hdr_ptr->active_fem_idx[idx] > max_active_fem_idx)
        {
          max_active_fem_idx = hdr_ptr->active_fem_idx[idx];
        }
        msg << " " << static_cast<int>(hdr_ptr->active_fem_idx[idx]);
      }
      LOG4CXX_TRACE(logger_, msg.str());
    }

    // Determine the size of the output reordered images
    unsigned int image_size = image_width_ * image_height_ * 2;
    LOG4CXX_TRACE(logger_, "Output image size: " << image_size);

    // Obtain a pointer to the start of the data in the frame
    const void* data_ptr = static_cast<const void*>(
        static_cast<const char*>(frame->get_data()) + sizeof(Lpd::FrameHeader)
    );

    // Pointers to reordered image buffer - will be allocated on demand
    void* reordered_image = NULL;

    try
    {

      // Allocate buffer to receive reordered image.
      reordered_image = (void*)malloc(image_pixels_ * 2);
      if (reordered_image == NULL)
      {
        throw std::runtime_error("Failed to allocate temporary buffer for reordered image");
      }


      // Calculate the FEM frame size once so it can be used in the following loop
      // repeatedly
      std::size_t fem_frame_size = Lpd::max_frame_size;

      // Loop over active FEMs in the input frame image data, reordering pixels into the output
      // images

      for (uint8_t idx = 0; idx < hdr_ptr->num_active_fems; idx++)
      {
        uint8_t fem_idx = hdr_ptr->active_fem_idx[idx];

        // Calculate pointer into the input image data based on loop index
        void* input_ptr = static_cast<void *>(
          static_cast<char *>(const_cast<void *>(data_ptr)) + (fem_frame_size * idx)
        );

        // Calculate output image pixel offset based on active FEM index
        std::size_t output_offset = fem_idx * (image_height_ * image_width_ * num_images_);

        // Determine stripe orientation based on FEM index
        bool stripe_is_even = ((fem_idx & 1) == 0);
        LOG4CXX_TRACE(logger_, "Active FEM idx=" << static_cast<int>(fem_idx)
            << ": stripe orientation is " << (stripe_is_even ? "even" : "odd"));

        unsigned int pkt_num = 0;
        unsigned int pkt_slot = hdr_ptr->fem_rx_state->packet_state[0][pkt_num];
        unsigned int pkt_offset = pkt_slot * (Lpd::primary_packet_size / 2);

        unsigned int pixel_offset = 0;
        unsigned int input_offset = 0;

        unsigned int num_pixels_per_row = Lpd::num_asic_cols * Lpd::num_pixel_cols_per_asic;

        // Loop over input pixel stream and re-order image to supermodule mapping (i.e. scatter
        // semantics). Note that row loops (pixel and ASIC) are reversed to recover correct image
        // orientation for supermodule
        for (unsigned int image = 0; image < num_images_; image++) //For each image
        {
          LOG4CXX_TRACE(logger_, "Reading Image: " << image);

          // For each pixel row per asic, starting from bottom
          for (int pixel_row = Lpd::num_pixel_rows_per_asic - 1; pixel_row >= 0; pixel_row--)
          {
            // For each pixel col per asic, starting from left
            for (unsigned int pixel_col = 0; pixel_col < Lpd::num_pixel_cols_per_asic; pixel_col++)
            {
              // For each asic row, starting from bottom
              for (int asic_row = Lpd::num_asic_rows - 1; asic_row >= 0; asic_row--)
              {
                unsigned int image_row = (asic_row * Lpd::num_pixel_rows_per_asic) + pixel_row;

                // For each each asic col, starting from left
                for (unsigned int asic_col = 0; asic_col < Lpd::num_asic_cols; asic_col++)
                 {
                   // Find next packet if pixel offset reaches the end of the current one
                   if (pixel_offset >= Lpd::primary_packet_size / 2)
                   {
                     uint16_t * input_data_ptr  = reinterpret_cast<uint16_t*>(input_ptr) + pkt_offset + (pixel_offset - 1);
                     pkt_num++;
                     pkt_slot = hdr_ptr->fem_rx_state->packet_state[0][pkt_num];
                     pkt_offset = pkt_slot * (Lpd::primary_packet_size / 2);
                     pixel_offset = 0;
                   }

                   //Input Offset
                   unsigned int input_offset = (Lpd::image_data_header / 2) + pkt_offset + pixel_offset;

                   // Output Offset
                   unsigned int image_col = (asic_col * Lpd::num_pixel_cols_per_asic) + pixel_col;
                   unsigned int output_offset = (image_row * num_pixels_per_row) + image_col;

                   uint16_t * output_data_ptr = reinterpret_cast<uint16_t*>(reordered_image) + output_offset;


                   // Copy Input to Output if packet is not missing
                   if (pkt_slot < 65535)
                   {
                     uint16_t * input_data_ptr  = reinterpret_cast<uint16_t*>(input_ptr) + input_offset;
                     *output_data_ptr = *input_data_ptr;
                   }
                   else // Fill output with zeros if packet missing
                   {
                     if (pixel_offset == 0) // Only log if first pixel
                     {
                       LOG4CXX_TRACE(logger_, "Pkt " << pkt_num << " missing, filling with 0");
                     }
                     *output_data_ptr = 0;
                   }
                   pixel_offset++;
                 }
               }
             }
           }
           // Set the frame image to the reordered image buffer if appropriate
           if (reordered_image)
           {
           // Pixel Data Dataset
           dimensions_t dims_data(2);
           dims_data[0] = image_height_;
           dims_data[1] = image_width_;

           boost::shared_ptr<Frame> data_frame;
           data_frame = boost::shared_ptr<Frame>(new Frame("data"));

           data_frame->set_frame_number(image_counter_);
           data_frame->set_dimensions("data", dims_data);

           void * output_data_ptr = static_cast<void*>(reordered_image);
           data_frame->copy_data(output_data_ptr, image_size);

           LOG4CXX_TRACE(logger_, "Pushing data dataset.");
           this->push(data_frame);

           // Image Number Dataset
           dimensions_t dims_img(1);
           dims_img[0] = 1;

           boost::shared_ptr<Frame> img_num_frame;
           img_num_frame = boost::shared_ptr<Frame>(new Frame("img_num"));

           img_num_frame->set_frame_number(image_counter_);
           img_num_frame->set_dimensions("img_num", dims_img);

           img_num_frame->copy_data(&image, 4);

           LOG4CXX_TRACE(logger_, "Pushing img_num dataset - " << image);
           this->push(img_num_frame);

           // Frame Number Dataset
           dimensions_t dims_frame(1);
           dims_frame[0] = 1;

           boost::shared_ptr<Frame> frame_num_frame;
           frame_num_frame = boost::shared_ptr<Frame>(new Frame("frame_num"));

           frame_num_frame->set_frame_number(image_counter_);
           frame_num_frame->set_dimensions("frame_num", dims_frame);

           frame_num_frame->copy_data(&(hdr_ptr->frame_number), 4);

           LOG4CXX_TRACE(logger_, "Pushing frame_num dataset - " << hdr_ptr->frame_number);
           this->push(frame_num_frame);
         }
         image_counter_++;
        }
        if(reordered_image)
        {
          free(reordered_image);
          reordered_image = NULL;
        }
      }
    }
    catch (const std::exception& e)
    {
      if(reordered_image)
      {
        free(reordered_image);
        reordered_image = NULL;
      }
      std::stringstream ss;
      ss << "LPD frame decode failed: " << e.what();
      LOG4CXX_ERROR(logger_, ss.str());
    }
  }
} /* namespace FrameProcessor */

