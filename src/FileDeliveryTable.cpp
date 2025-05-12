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
#include "FileDeliveryTable.h"
#include "tinyxml2.h"
#include <iostream>
#include <string>
#include <map>
#include "spdlog/spdlog.h"

namespace {
  class XMLNamespaces {
  public:
    XMLNamespaces() :_default_ns() ,_prefix_to_ns_map() {};
    XMLNamespaces(const XMLNamespaces &to_copy) :_default_ns(to_copy._default_ns) ,_prefix_to_ns_map(to_copy._prefix_to_ns_map) {};
    XMLNamespaces(XMLNamespaces &&to_move) :_default_ns(std::move(to_move._default_ns)) ,_prefix_to_ns_map(std::move(to_move._prefix_to_ns_map)) {};
    XMLNamespaces(const tinyxml2::XMLElement *element, const XMLNamespaces &parent_ns = XMLNamespaces());

    const tinyxml2::XMLElement *findChildElement(const tinyxml2::XMLElement *element, const std::string &name, const std::string &ns = std::string()) const;
    const tinyxml2::XMLElement *findSiblingElement(const tinyxml2::XMLElement *child_elem, const std::string &name, const std::string &ns = std::string()) const;
    const tinyxml2::XMLAttribute *findAttribute(const tinyxml2::XMLElement *element, const std::string &name, const std::string &ns = std::string()) const;
    bool matches(const std::string &prefixed_name, const std::string &name, const std::string &ns = std::string()) const;
    const std::string &elementNamespace(const tinyxml2::XMLElement *element) const;

  private:
    std::string _default_ns;   // namespace given for "xmlns=..."
    std::map<std::string, std::string> _prefix_to_ns_map; // namespaces given for "xmlns:prefix=..."
  };

  XMLNamespaces::XMLNamespaces(const tinyxml2::XMLElement *element, const XMLNamespaces &parent_ns)
    :_default_ns(parent_ns._default_ns)
    ,_prefix_to_ns_map(parent_ns._prefix_to_ns_map)
  {
    if (!element) return;
    for (auto attr_ptr = element->FirstAttribute(); attr_ptr; attr_ptr = attr_ptr->Next()) {
      std::string attr_name = attr_ptr->Name();
      if (attr_name == "xmlns") {
        _default_ns = std::string(attr_ptr->Value());
      } else if (attr_name.substr(0,6) == "xmlns:") {
        _prefix_to_ns_map.insert(std::make_pair(attr_name.substr(6), std::string(attr_ptr->Value())));
      }
    }
  }

  const tinyxml2::XMLElement *XMLNamespaces::findChildElement(const tinyxml2::XMLElement *element, const std::string &name, const std::string &ns) const
  {
    if (!element) return nullptr;
    auto elem_ptr = element->FirstChildElement();
    if (!elem_ptr) return nullptr;
    XMLNamespaces child_ns(elem_ptr, *this);
    if (child_ns.matches(elem_ptr->Name(), name, ns)) return elem_ptr;
    return findSiblingElement(elem_ptr, name, ns);
  }

  const tinyxml2::XMLElement *XMLNamespaces::findSiblingElement(const tinyxml2::XMLElement *child_elem, const std::string &name, const std::string &ns) const
  {
    if (!child_elem) return nullptr;
    for (auto elem_ptr = child_elem->NextSiblingElement(); elem_ptr; elem_ptr = elem_ptr->NextSiblingElement()) {
      XMLNamespaces child_ns(elem_ptr, *this);
      if (child_ns.matches(elem_ptr->Name(), name, ns)) return elem_ptr;
    }
    return nullptr;
  }

  const tinyxml2::XMLAttribute *XMLNamespaces::findAttribute(const tinyxml2::XMLElement *element, const std::string &name, const std::string &ns) const
  {
    const std::string &elem_ns = elementNamespace(element);
    for (auto attr_ptr = element->FirstAttribute(); attr_ptr; attr_ptr = attr_ptr->Next()) {
      XMLNamespaces attr_ns(*this);
      attr_ns._default_ns = elem_ns;
      if (attr_ns.matches(attr_ptr->Name(), name, ns)) return attr_ptr;
    }
    return nullptr;
  }

  bool XMLNamespaces::matches(const std::string &prefixed_name, const std::string &name, const std::string &ns) const
  {
    std::string match_ns(_default_ns);
    std::string match_name(prefixed_name);
    auto pos = match_name.find_first_of(':');
    if (pos != std::string::npos) {
      auto prefix = match_name.substr(0,pos);
      match_name.erase(0,pos+1);
      match_ns = _prefix_to_ns_map.at(prefix);
    }
    return name == match_name && ns == match_ns;
  }

  const std::string &XMLNamespaces::elementNamespace(const tinyxml2::XMLElement *element) const
  {
    std::string elem_name(element->Name());
    auto pos = elem_name.find_first_of(':');
    if (pos != std::string::npos) {
      auto elem_prefix = elem_name.substr(0,pos);
      return _prefix_to_ns_map.at(elem_prefix);
    }
    return _default_ns;
  }
}

LibFlute::FileDeliveryTable::FileDeliveryTable(uint32_t instance_id, FecOti fec_oti, FdtNamespace fdt_namespace)
  : _instance_id( instance_id )
  , _global_fec_oti( fec_oti )
  , _fdt_namespace( fdt_namespace )
{
}

LibFlute::FileDeliveryTable::FileDeliveryTable(uint32_t instance_id, char* buffer, size_t len) 
  : _instance_id( instance_id )
{
  static const std::string mbms2007_ns("urn:3GPP:metadata:2007:MBMS:FLUTE:FDT");
  tinyxml2::XMLDocument doc(true, tinyxml2::COLLAPSE_WHITESPACE);
  doc.Parse(buffer, len);
  auto fdt_instance = doc.RootElement();
  XMLNamespaces root_ns(fdt_instance);
  auto fdt_ns = root_ns.elementNamespace(fdt_instance);
  if (!root_ns.matches(fdt_instance->Name(), "FDT-Instance", fdt_ns)) {
    throw "Root element is not FDT-Instance";
  }

  if (fdt_ns == "") {
    _fdt_namespace = FDT_NS_NONE;
  } else if (fdt_ns == "http://www.example.com/flute") {
    _fdt_namespace = FDT_NS_RFC3926;
  } else if (fdt_ns == "urn:IETF:metadata:2005:FLUTE:FDT") {
    _fdt_namespace = FDT_NS_DRAFT_2005;
//  } else if (fdt_ns == "urn:ietf:params:xml:ns:fdt") { // FLUTEv2 - needs more work
//    _fdt_namespace = FDT_NS_RFC6726;
  } else if (fdt_ns == "urn:3GPP:metadata:2022:FLUTE:FDT") {
    _fdt_namespace = FDT_NS_3GPP_CONSOLIDATED_V2;
  } else {
    throw "FDT namespace not recognised";
  }

  _expires = std::stoull(root_ns.findAttribute(fdt_instance, "Expires", fdt_ns)->Value());

  spdlog::debug("Received new FDT with instance ID {}: {}", instance_id, buffer);

  uint8_t def_fec_encoding_id = 0;
  auto val = root_ns.findAttribute(fdt_instance, "FEC-OTI-FEC-Encoding-ID", fdt_ns);
  if (val != nullptr) {
    def_fec_encoding_id = strtoul(val->Value(), nullptr, 0);
  }

  uint32_t def_fec_max_source_block_length = 0;
  val = root_ns.findAttribute(fdt_instance, "FEC-OTI-Maximum-Source-Block-Length", fdt_ns);
  if (val != nullptr) {
    def_fec_max_source_block_length = strtoul(val->Value(), nullptr, 0);
  }

  uint32_t def_fec_encoding_symbol_length = 0;
  val = root_ns.findAttribute(fdt_instance, "FEC-OTI-Encoding-Symbol-Length", fdt_ns);
  if (val != nullptr) {
    def_fec_encoding_symbol_length = strtoul(val->Value(), nullptr, 0);
  }

  for (auto file = root_ns.findChildElement(fdt_instance, "File", fdt_ns);
      file != nullptr; file = root_ns.findSiblingElement(file, "File", fdt_ns)) {

    XMLNamespaces file_ns(file, root_ns);

    // required attributes
    auto toi_str = file_ns.findAttribute(file, "TOI", fdt_ns);
    if (toi_str == nullptr) {
      throw "Missing TOI attribute on File element";
    }
    uint32_t toi = strtoull(toi_str->Value(), nullptr, 0);

    auto content_location = file_ns.findAttribute(file, "Content-Location", fdt_ns);
    if (content_location == nullptr) {
      throw "Missing Content-Location attribute on File element";
    }

    uint32_t content_length = 0;
    val = file_ns.findAttribute(file, "Content-Length", fdt_ns);
    if (val != nullptr) {
      content_length = strtoull(val->Value(), nullptr, 0);
    }

    uint32_t transfer_length = 0;
    val = file_ns.findAttribute(file, "Transfer-Length", fdt_ns);
    if (val != nullptr) {
      transfer_length = strtoull(val->Value(), nullptr, 0);
    } else {
      transfer_length = content_length;
    }

    auto content_md5 = file_ns.findAttribute(file, "Content-MD5", fdt_ns)->Value();
    if (!content_md5) {
      content_md5 = "";
    }

    auto content_type = file_ns.findAttribute(file, "Content-Type", fdt_ns)->Value();
    if (!content_type) {
      content_type = "";
    }

    auto encoding_id = def_fec_encoding_id;
    val = file_ns.findAttribute(file, "FEC-OTI-FEC-Encoding-ID", fdt_ns);
    if (val != nullptr) {
      encoding_id = strtoul(val->Value(), nullptr, 0);
    }

    auto max_source_block_length = def_fec_max_source_block_length;
    val = file_ns.findAttribute(file, "FEC-OTI-Maximum-Source-Block-Length", fdt_ns);
    if (val != nullptr) {
      max_source_block_length = strtoul(val->Value(), nullptr, 0);
    }

    auto encoding_symbol_length = def_fec_encoding_symbol_length;
    val = file_ns.findAttribute(file, "FEC-OTI-Encoding-Symbol-Length", fdt_ns);
    if (val != nullptr) {
      encoding_symbol_length = strtoul(val->Value(), nullptr, 0);
    }
    uint32_t expires = 0;
    auto cc = file_ns.findChildElement(file, "Cache-Control", mbms2007_ns);
    if (cc) {
      XMLNamespaces cc_ns(cc, file_ns);
      auto expires_elem = cc_ns.findChildElement(cc, "Expires", mbms2007_ns);
      if (expires_elem) {
        expires = strtoul(expires_elem->GetText(), nullptr, 0);
      }
    }

    FecOti fec_oti{
      (FecScheme)encoding_id,
        transfer_length,
        encoding_symbol_length,
        max_source_block_length
    };

    FileEntry fe{
      toi,
        std::string(content_location->Value()),
        content_length,
        std::string(content_md5),
        std::string(content_type),
        expires,
        fec_oti
    };
    _file_entries.push_back(fe);
  }
}

auto LibFlute::FileDeliveryTable::add(const FileEntry& fe) -> void
{
  _instance_id++;
  _file_entries.push_back(fe);
}

auto LibFlute::FileDeliveryTable::remove(uint32_t toi) -> void
{
  for (auto it = _file_entries.cbegin(); it != _file_entries.cend();) {
    if (it->toi == toi) {
      it = _file_entries.erase(it);
    } else {
      ++it;
    }
  }
  _instance_id++;
}

auto LibFlute::FileDeliveryTable::to_string() const -> std::string {
  tinyxml2::XMLDocument doc;
  doc.InsertFirstChild( doc.NewDeclaration() );
  auto root = doc.NewElement("FDT-Instance");
  switch (_fdt_namespace) {
    case FDT_NS_RFC3926:
      root->SetAttribute("xmlns", "http://www.example.com/flute");
      break;
    case FDT_NS_DRAFT_2005:
      root->SetAttribute("xmlns", "urn:IETF:metadata:2005:FLUTE:FDT");
      break;
//    case FDT_NS_RFC6726:  // FLUTE v2 - Will need other things implementing to use this
//      root->SetAttribute("xmlns", "urn:ietf:params:xml:ns:fdt");
//      break;
    case FDT_NS_3GPP_CONSOLIDATED_V2:
      root->SetAttribute("xmlns", "urn:3GPP:metadata:2022:FLUTE:FDT");
      break;
    default:
      break;
  }
  root->SetAttribute("Expires", std::to_string(_expires).c_str());
  root->SetAttribute("FEC-OTI-FEC-Encoding-ID", (unsigned)_global_fec_oti.encoding_id);
  root->SetAttribute("FEC-OTI-Maximum-Source-Block-Length", (unsigned)_global_fec_oti.max_source_block_length);
  root->SetAttribute("FEC-OTI-Encoding-Symbol-Length", (unsigned)_global_fec_oti.encoding_symbol_length);
  root->SetAttribute("xmlns:mbms2007", "urn:3GPP:metadata:2007:MBMS:FLUTE:FDT");
  doc.InsertEndChild(root);

  for (const auto& file : _file_entries) {
    auto f = doc.NewElement("File");
    f->SetAttribute("TOI", file.toi);
    f->SetAttribute("Content-Location", file.content_location.c_str());
    f->SetAttribute("Content-Length", file.content_length);
    f->SetAttribute("Transfer-Length", (unsigned)file.fec_oti.transfer_length);
    f->SetAttribute("Content-MD5", file.content_md5.c_str());
    f->SetAttribute("Content-Type", file.content_type.c_str());
    auto cc = doc.NewElement("mbms2007:Cache-Control");
    auto exp = doc.NewElement("mbms2007:Expires");
    exp->SetText(std::to_string(file.expires).c_str());
    cc->InsertEndChild(exp);
    f->InsertEndChild(cc);
    root->InsertEndChild(f);
  }


  tinyxml2::XMLPrinter printer;
  doc.Print(&printer);
  return std::string(printer.CStr());
}
