
// The code in this file is released into the Public Domain.

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>

#include <gdal_priv.h>
#include <cpl_conv.h>
#include <cpl_string.h>
#include <ogr_spatialref.h>

#include <osmium/io/any_input.hpp>
#include <osmium/handler.hpp>
#include <osmium/visitor.hpp>
#include <osmium/geom/mercator_projection.hpp>
#include <osmium/geom/projection.hpp>

#include "cmdline_options.hpp"

typedef uint32_t node_count_type;

class NodeDensityHandler : public osmium::handler::Handler {

    const Options& m_options;
    const osmium::Box m_box;

    osmium::geom::Projection m_projection;

    const int m_width;
    const int m_height;

    const osmium::geom::Coordinates m_bottom_left;
    const osmium::geom::Coordinates m_top_right;

    const double m_factor_x;
    const double m_factor_y;

    std::unique_ptr<node_count_type[]> m_node_count;

    template <typename T>
    static constexpr T in_range(T min, T value, T max) {
        return std::min(std::max(value, min), max);
    }

public:

    NodeDensityHandler(const Options& options, const osmium::Box& box) :
        m_options(options),
        m_box(box),
        m_projection(options.epsg),
        m_width(options.width),
        m_height(options.height),
        m_bottom_left(m_projection(box.bottom_left())),
        m_top_right(m_projection(box.top_right())),
        m_factor_x(m_width  / (m_top_right.x - m_bottom_left.x)),
        m_factor_y(m_height / (m_top_right.y - m_bottom_left.y)),
        m_node_count(new node_count_type[m_width * m_height]) {
    }

    void node(const osmium::Node& node) {
        if (m_box.contains(node.location())) {
            osmium::geom::Coordinates c = m_projection(node.location());

            const int x = in_range(0, static_cast<int>(( c.x - m_bottom_left.x) * m_factor_x), m_width - 1);
            const int y = in_range(0, static_cast<int>((-c.y - m_bottom_left.y) * m_factor_y), m_height - 1);
            const int n = y * m_width + x;
            ++m_node_count[n];
        }
    }

    void write_to_file() {
        GDALAllRegister();

        GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
        if (!driver) {
            std::cerr << "Can't initalize GDAL GTiff driver.\n";
            exit(return_code::fatal);
        }

        std::vector<std::string> options;
        options.push_back("COMPRESS=" + m_options.compression_format);
        options.push_back("TILED=YES");

        auto dataset_options = std::unique_ptr<char*[]>(new char*[options.size()+1]);
        std::transform(options.begin(), options.end(), dataset_options.get(), [&](const std::string& s) {
            return const_cast<char*>(s.data());
        });
        dataset_options[options.size()] = nullptr;

        GDALDataset* dataset = driver->Create(m_options.output_filename.c_str(), m_width, m_height, 1, GDT_UInt32, dataset_options.get());
        if (!dataset) {
            std::cerr << "Can't create output file '" << m_options.output_filename <<"'.\n";
            exit(return_code::error);
        }

        dataset->SetMetadataItem("TIFFTAG_IMAGEDESCRIPTION", "OpenStreetMap node density");
        dataset->SetMetadataItem("TIFFTAG_COPYRIGHT", "Copyright OpenStreetMap contributors (http://www.openstreetmap.org/copyright), License: CC-BY-SA (http://creativecommons.org/licenses/by-sa/2.0/)");
        dataset->SetMetadataItem("TIFFTAG_SOFTWARE", "node_density");

        double geo_transform[6] = {m_bottom_left.x, 1/m_factor_x, 0, m_top_right.y, 0, -1/m_factor_y};
        dataset->SetGeoTransform(geo_transform);

        {
            OGRSpatialReference srs;
            srs.importFromProj4(m_projection.proj_string().c_str());
            char* wkt = nullptr;
            srs.exportToWkt(&wkt);
            dataset->SetProjection(wkt);
            CPLFree(wkt);
        }

        GDALRasterBand* band = dataset->GetRasterBand(1);
        assert(band);
        if (band->RasterIO(GF_Write, 0, 0, m_width, m_height, m_node_count.get(), m_width, m_height, GDT_UInt32, 0, 0) != CE_None) {
            std::cerr << "Error writing to output file '" << m_options.output_filename <<"'.\n";
            exit(return_code::error);
        }

        if (m_options.build_overview) {
            int num = std::min(static_cast<int>(std::log2(m_width / 256.0)), 8);
            int overview_list[] = { 2, 4, 8, 16, 32, 64, 128, 256 };
            dataset->BuildOverviews("AVERAGE", num, overview_list, 0, nullptr, nullptr, nullptr);
        }

        GDALClose(dataset);
    }

}; // class NodeDensityHandler

int main(int argc, char* argv[]) {
    Options options(argc, argv);

    options.vout << "Options from command line or defaults:\n";
    options.vout << "  Input file:                  " << options.input_filename << "\n";
    if (!options.input_format.empty()) {
        options.vout << "  Input format:                " << options.input_format << "\n";
    }
    options.vout << "  Coordinate Reference System: " << options.epsg << "\n";
    options.vout << "  Output file:                 " << options.output_filename << "\n";
    options.vout << "  Compression:                 " << options.compression_format << "\n";
    options.vout << "  Pixel width:                 " << options.width << "\n";
    options.vout << "  Pixel height:                " << options.height << "\n";
    options.vout << "  Build overviews:             " << (options.build_overview ? "YES" : "NO") << "\n";

    osmium::Box bounding_box;

    if (options.epsg == 3857) {
        bounding_box.extend(osmium::Location(-180.0, -osmium::geom::MERCATOR_MAX_LAT));
        bounding_box.extend(osmium::Location( 180.0,  osmium::geom::MERCATOR_MAX_LAT));
    } else {
        bounding_box.extend(osmium::Location(-180.0, -90.0));
        bounding_box.extend(osmium::Location( 180.0,  90.0));
    }

    NodeDensityHandler handler(options, bounding_box);

    osmium::io::File file(options.input_filename, options.input_format);
    osmium::io::Reader reader(file, osmium::osm_entity_bits::node);

    options.vout << "Counting nodes...\n";
    osmium::apply(reader, handler);
    options.vout << "Done.\n";

    options.vout << "Writing image to output file...\n";
    handler.write_to_file();
    options.vout << "Done.\n";

    google::protobuf::ShutdownProtobufLibrary();
}

