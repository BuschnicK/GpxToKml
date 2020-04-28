#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "boost/algorithm/string/case_conv.hpp"
#include "boost/algorithm/string/trim.hpp"
#include "boost/asio.hpp"
#include "boost/filesystem.hpp"
#include "boost/lexical_cast.hpp"
#include "boost/nowide/fstream.hpp"
#include "boost/program_options.hpp"
#include "boost/regex.hpp"
#include "boost/thread/thread.hpp"
#include "tinyxml2/tinyxml2.h"

namespace {

struct Coordinate {
  double lat;
  double lon;
  double alt;
};

using Coordinates = std::vector<Coordinate>;

std::tm ParseTime(const tinyxml2::XMLElement& root) {
  const tinyxml2::XMLElement* element = root.FirstChildElement("metadata");
  if (!element) {
    throw std::invalid_argument("Missing metadata element");
  }
  element = element->FirstChildElement("time");
  if (!element) {
    throw std::invalid_argument("Missing metadata time element");
  }
  std::istringstream time_stream(element->GetText());
  std::tm time;
  time_stream >> std::get_time(&time, "%Y-%m-%dT%H:%M:%SZ");
  if (time_stream.fail()) {
    throw std::invalid_argument(element->GetText());
  }
  return time;
}

std::string ParseName(const tinyxml2::XMLElement& track) {
  const tinyxml2::XMLElement* name = track.FirstChildElement("name");
  if (!name) {
    throw std::invalid_argument("Missing name element");
  }
  return name->GetText();
}

Coordinates ParseCoordinates(const tinyxml2::XMLElement& track) {
  const tinyxml2::XMLElement* segment = track.FirstChildElement("trkseg");
  if (!segment) {
    throw std::invalid_argument("Missing trkseg element");
  }

  Coordinates coordinates;
  for (const tinyxml2::XMLElement* point = segment->FirstChildElement("trkpt");
       point; point = point->NextSiblingElement("trkpt")) {
    const tinyxml2::XMLAttribute* lat = point->FindAttribute("lat");
    const tinyxml2::XMLAttribute* lon = point->FindAttribute("lon");
    if (!lat || !lon) {
      throw std::invalid_argument("Missing lat/lon attributes");
    }
    const tinyxml2::XMLElement* elevation = point->FirstChildElement("ele");
    if (!elevation) {
      throw std::invalid_argument("Missing ele element");
    }
    coordinates.push_back(
        Coordinate({.lat = boost::lexical_cast<double>(lat->Value()),
                    .lon = boost::lexical_cast<double>(lon->Value()),
                    .alt = boost::lexical_cast<double>(elevation->GetText())}));
  }
  return coordinates;
}

std::string NormalizeFilename(const std::string& filename) {
  // List of illegal characters: https://stackoverflow.com/a/31976060
  return boost::algorithm::trim_copy(
      boost::regex_replace(filename, boost::regex(R"([<>:"\/\|\?\*])"), "_"));
}

void WriteFile(std::string_view name, const std::tm& time,
               const Coordinates& coordinates,
               const boost::filesystem::path& output_dir) {
  std::stringstream basename;
  basename << std::put_time(&time, "%Y-%m-%d") << " " << name;
  std::stringstream filename;
  filename << basename.str() << ".kml";

  tinyxml2::XMLDocument xml_doc;
  xml_doc.InsertEndChild(xml_doc.NewDeclaration());

  tinyxml2::XMLElement* root = xml_doc.NewElement("kml");
  root->SetAttribute("xmlns", "http://www.opengis.net/kml/2.2");
  root->SetAttribute("xmlns:gx", "http://www.google.com/kml/ext/2.2");
  root->SetAttribute("xmlns:kml", "http://www.opengis.net/kml/2.2");
  root->SetAttribute("xmlns:atom", "http://www.w3.org/2005/Atom");
  tinyxml2::XMLElement* document = root->InsertNewChildElement("Document");
  document->InsertNewChildElement("name")->SetText(filename.str().data());
  tinyxml2::XMLElement* style = document->InsertNewChildElement("Style");
  style->SetAttribute("id", "style1");
  tinyxml2::XMLElement* line_style = style->InsertNewChildElement("LineStyle");
  line_style->InsertNewChildElement("color")->SetText("ff0000ff");
  line_style->InsertNewChildElement("width")->SetText("4");
  tinyxml2::XMLElement* style_map = document->InsertNewChildElement("StyleMap");
  style_map->SetAttribute("id", "stylemap_id00");
  tinyxml2::XMLElement* pair = style_map->InsertNewChildElement("Pair");
  pair->InsertNewChildElement("key")->SetText("normal");
  pair->InsertNewChildElement("styleUrl")->SetText("style1");
  pair = style_map->InsertNewChildElement("Pair");
  pair->InsertNewChildElement("key")->SetText("highlight");
  pair->InsertNewChildElement("styleUrl")->SetText("style1");

  tinyxml2::XMLElement* place = document->InsertNewChildElement("Placemark");
  place->InsertNewChildElement("name")->SetText(basename.str().data());
  place->InsertNewChildElement("styleUrl")->SetText("#stylemap_id00");

  std::stringstream coordinate_string;
  coordinate_string.precision(7);
  for (const Coordinate& coordinate : coordinates) {
    coordinate_string << std::fixed << coordinate.lon << "," << coordinate.lat
                      << "," << coordinate.alt << " ";
  }
  place->InsertNewChildElement("MultiGeometry")
      ->InsertNewChildElement("LineString")
      ->InsertNewChildElement("coordinates")
      ->SetText(coordinate_string.str().data());
  xml_doc.InsertEndChild(root);

  const boost::filesystem::path output_path =
      output_dir / NormalizeFilename(filename.str());

  std::shared_ptr<FILE> file(
      boost::nowide::fopen(output_path.string().data(), "w"), fclose);
  if (xml_doc.SaveFile(file.get()) != tinyxml2::XML_SUCCESS) {
    throw std::invalid_argument(std::string("Failed writing to: ") +
                                output_path.string());
  }
  std::cout << "Writing: " << output_path.string() << std::endl;
}

void ConvertFile(std::string_view input_file,
                 const boost::filesystem::path& output_dir) {
  tinyxml2::XMLDocument xml_doc;
  if (xml_doc.LoadFile(input_file.data()) != tinyxml2::XML_SUCCESS) {
    throw std::invalid_argument(xml_doc.ErrorStr());
  }
  const tinyxml2::XMLElement* root = xml_doc.FirstChildElement("gpx");
  if (!root) {
    throw std::invalid_argument("Missing root element");
  }

  const std::tm time = ParseTime(*root);

  const tinyxml2::XMLElement* track = root->FirstChildElement("trk");
  if (!track) {
    throw std::invalid_argument("Missing trk element");
  }

  const std::string name = ParseName(*track);
  const Coordinates coordinates = ParseCoordinates(*track);

  WriteFile(name, time, coordinates, output_dir);
}

void Main(std::string_view input_dir, std::string_view output_dir_string) {
  const boost::filesystem::path output_dir(output_dir_string.data());
  if (!boost::filesystem::is_directory(output_dir)) {
    throw std::invalid_argument(std::string("Not a directory: ") +
                                output_dir.string());
  }

  boost::asio::io_service io_service;
  boost::asio::io_service::work work(io_service);
  boost::thread_group threads;
  for (std::size_t i = 0; i < std::thread::hardware_concurrency(); ++i) {
    threads.create_thread(
        boost::bind(&boost::asio::io_service::run, &io_service));
  }

  for (boost::filesystem::directory_entry& entry :
       boost::filesystem::directory_iterator(input_dir.data())) {
    if (!boost::filesystem::is_regular_file(entry)) {
      continue;
    }
    if (boost::algorithm::to_lower_copy(boost::filesystem::extension(entry)) !=
        ".gpx") {
      continue;
    }
    std::cout << "Enqueuing: " << entry << std::endl;
    io_service.post([entry, output_dir]() {
      try {
        ConvertFile(entry.path().string(), output_dir);
      } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << std::endl;
      }
    });
  }

  io_service.stop();
  threads.join_all();
}

}  // namespace

int main(int argc, char** argv) {
  try {
    boost::program_options::options_description flags_description(
        "Supported options");
    flags_description.add_options()("help", "List command line options")(
        "input_dir", boost::program_options::value<std::string>(),
        "Glob matching input GPX files.")(
        "output_dir", boost::program_options::value<std::string>(),
        "Output directory for KML results.");

    boost::program_options::variables_map flags;
    boost::program_options::store(boost::program_options::parse_command_line(
                                      argc, argv, flags_description),
                                  flags);
    boost::program_options::notify(flags);

    if (flags.count("help") || flags.empty()) {
      std::cout << flags_description << std::endl;
      return EXIT_SUCCESS;
    }
    if (!flags.count("input_dir") || !flags.count("output_dir")) {
      std::cout << "input_dir and output_dir must be provided!\n";
      std::cout << flags_description << std::endl;
      return EXIT_FAILURE;
    }

    Main(flags["input_dir"].as<std::string>(),
         flags["output_dir"].as<std::string>());
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << std::endl;
    return EXIT_FAILURE;
  } catch (...) {
    std::cerr << "Unknown error." << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
