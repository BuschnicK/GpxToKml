#include <SDKDDKVer.h>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <syncstream>
#include <thread>
#include <vector>

#include "boost/algorithm/string/case_conv.hpp"
#include "boost/algorithm/string/trim.hpp"
#include "boost/asio.hpp"
#include "boost/filesystem.hpp"
#include "boost/format.hpp"
#include "boost/lexical_cast.hpp"
#include "boost/nowide/args.hpp"
#include "boost/nowide/filesystem.hpp"
#include "boost/nowide/fstream.hpp"
#include "boost/nowide/iostream.hpp"
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
  const boost::filesystem::path output_path =
      output_dir / NormalizeFilename(filename.str());
  if (boost::filesystem::exists(output_path)) {
    throw std::invalid_argument(
        boost::str(boost::format("Output file already exists, skipping \"%s\"") % output_path.string()));
  }
  std::osyncstream(boost::nowide::cout) << "Writing: " << output_path << std::endl;
  std::shared_ptr<FILE> file(
      boost::nowide::fopen(output_path.string().data(), "w"), fclose);

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

  if (xml_doc.SaveFile(file.get()) != tinyxml2::XML_SUCCESS) {
    throw std::invalid_argument(
        boost::str(boost::format("Failed writing to: \"%s\"") % output_path));
  }
}

void ConvertFile(std::string_view input_file,
                 const boost::filesystem::path& output_dir) {
  try {
    tinyxml2::XMLDocument xml_doc;
    if (xml_doc.LoadFile(input_file.data()) != tinyxml2::XML_SUCCESS) {
      throw std::invalid_argument(boost::str(
          boost::format("Failed reading XML file %s") % xml_doc.ErrorStr()));
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
  } catch (const std::exception& error) {
    throw std::invalid_argument(
        boost::str(boost::format("%s while parsing: \"%s\"") % error.what() % input_file));
  }
}

void Main(std::string_view input_dir,
          std::optional<std::string_view> output_dir_string) {
  if (!output_dir_string.has_value()) {
    output_dir_string = input_dir;
  }
  const boost::filesystem::path output_dir(output_dir_string->data());
  if (!boost::filesystem::is_directory(output_dir)) {
    throw std::invalid_argument(boost::str(boost::format("Not a directory: \"%s\"") %
                                *output_dir_string));
  }

  boost::asio::io_service io_service;
  boost::asio::io_service::work work(io_service);
  boost::thread_group threads;
  for (std::size_t i = 0; i < std::thread::hardware_concurrency(); ++i) {
    threads.create_thread(
        boost::bind(&boost::asio::io_service::run, &io_service));
  }

  std::atomic<int> num_processed_successfully = 0;
  std::atomic<int> num_failed = 0;  
  std::atomic<int> num_in_progress = 0;
  std::mutex mutex;
  std::condition_variable busy;
  for (boost::filesystem::directory_entry& entry :
       boost::filesystem::directory_iterator(input_dir.data())) {
    if (!boost::filesystem::is_regular_file(entry)) {
      continue;
    }
    if (boost::algorithm::to_lower_copy(entry.path().extension().string()) !=
        ".gpx") {
      continue;
    }
    std::osyncstream(boost::nowide::cout) << "Reading: " << entry << std::endl;

    std::unique_lock<std::mutex> lock(mutex);
    // Rate limit the work queue to twice the number of tasks as threads.
    while (num_in_progress >= threads.size() * 2) {
      busy.wait(lock);
    }
    ++num_in_progress;

    io_service.post([entry, output_dir, &num_processed_successfully,
                     &num_failed, &num_in_progress, &busy]() {
      try {
        ConvertFile(entry.path().string(), output_dir);
        ++num_processed_successfully;
        --num_in_progress;
        busy.notify_one();
      } catch (const std::exception& error) {
        std::osyncstream(boost::nowide::cerr) << "error: " << error.what() << std::endl;
        ++num_failed;
        --num_in_progress;
        busy.notify_one();
      }
    });
  }

  io_service.stop();
  threads.join_all();
  boost::nowide::cout << "Succeeded: " << num_processed_successfully
            << " Failed: " << num_failed << std::endl;
}

}  // namespace

int main(int argc, char** argv) {
  boost::nowide::nowide_filesystem();
  boost::nowide::args _(argc, argv);  // Fix arguments - make them UTF-8
  try {
    boost::program_options::options_description flags_description(
        "Supported options");
    flags_description.add_options()("help", "List command line options")(
        "input_dir", boost::program_options::value<std::string>(),
        "Input directory containing GPX files.")(
        "output_dir", boost::program_options::value<std::string>(),
        "Output directory for KML results. Defaults to input_dir.");

    boost::program_options::variables_map flags;
    boost::program_options::store(boost::program_options::parse_command_line(
                                      argc, argv, flags_description),
                                  flags);
    boost::program_options::notify(flags);

    if (flags.count("help") || flags.empty()) {
      boost::nowide::cout << flags_description << std::endl;
      return EXIT_SUCCESS;
    }
    if (!flags.count("input_dir")) {
      boost::nowide::cout << "input_dir must be provided!\n";
      boost::nowide::cout << flags_description << std::endl;
      return EXIT_FAILURE;
    }
    std::optional<std::string> output_dir;
    if (flags.contains("output_dir")) {
      output_dir = flags["output_dir"].as<std::string>();
    }
    Main(flags["input_dir"].as<std::string>(), output_dir);
  } catch (const std::exception& error) {
    boost::nowide::cerr << "error: " << error.what() << std::endl;
    return EXIT_FAILURE;
  } catch (...) {
    boost::nowide::cerr << "Unknown error." << std::endl;
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
