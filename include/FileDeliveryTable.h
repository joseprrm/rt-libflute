// libflute - FLUTE/ALC library
//
// Copyright (C) 2021 Klaus Kühnhammer (Österreichische Rundfunksender GmbH & Co KG)
//               2025 British Broadcasting Corporation (David Waring <david.waring2@bbc.co.uk>)
//
// Licensed under the License terms and conditions for use, reproduction, and
// distribution of 5G-MAG software (the “License”).  You may not use this file
// except in compliance with the License.  You may obtain a copy of the License at
// https://www.5g-mag.com/reference-tools.  Unless required by applicable law or
// agreed to in writing, software distributed under the License is distributed on
// an “AS IS” BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.
//
// See the License for the specific language governing permissions and limitations
// under the License.
//
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>
#include "flute_types.h"

namespace LibFlute {
  /**
   *  A class for parsing and creating FLUTE FDTs (File Delivery Tables).
   */
  class FileDeliveryTable {
    public:
     /**
      * FDT namespace enumeration
      */
      enum FdtNamespace {
          FDT_NS_NONE = 0,
          FDT_NS_RFC3926,
          FDT_NS_DRAFT_2005,
//          FDT_NS_RFC6726, // FLUTE v2 - will need other things implementing to use this correctly
          FDT_NS_3GPP_CONSOLIDATED_V2
      };

     /**
      *  Create an empty FDT
      *
      *  @param instance_id FDT instance ID to set
      *  @param fec_oti Global FEC OTI parameters
      *  @param fdt_namespace The XML namespace to use for FDT
      */
      FileDeliveryTable(uint32_t instance_id, FecOti fec_oti, FdtNamespace fdt_namespace = FDT_NS_NONE);

     /**
      *  Parse an XML string and create a FDT class from it
      *
      *  @param instance_id FDT instance ID (from ALC headers)
      *  @param buffer String containing the FDT XML
      *  @param len Length of the buffer
      */
      FileDeliveryTable(uint32_t instance_id, char* buffer, size_t len);

     /**
      *  Default destructor.
      */
      virtual ~FileDeliveryTable() {};

     /**
      *  Get the FDT instance ID
      */
      uint32_t instance_id() { return _instance_id; };

     /**
      *  An entry for a file in the FDT
      */
      struct FileEntry {
        uint32_t toi;
        std::string content_location;
        uint32_t content_length;
        std::string content_md5;
        std::string content_type;
        uint64_t expires;
        FecOti fec_oti;
      };

     /**
      *  Set the expiry value
      */
      void set_expires(uint64_t exp) { _expires = exp; };

     /**
      *  Add a file entry
      */
      void add(const FileEntry& entry);

     /**
      *  Remove a file entry
      */
      void remove(uint32_t toi);

     /**
      *  Serialize the FDT to an XML string
      */
      std::string to_string() const;

     /**
      *  Get all current file entries
      */
      std::vector<FileEntry> file_entries() { return _file_entries; };

    private:
      uint32_t _instance_id;

      std::vector<FileEntry> _file_entries;
      FecOti _global_fec_oti;

      uint64_t _expires;

      FdtNamespace _fdt_namespace;
  };
};
