#define TINY_DNG_LOADER_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define TINY_DNG_WRITER_IMPLEMENTATION
#include <cstdint> // Add uint32_t for tiny_dng_loader.h:47:7
#include <iostream>
#include "tiny_dng_loader.h"
#include "tiny_dng_writer.h"
#include "stb_image_write.h"

std::vector<tinydng::DNGImage> readDNG(std::string filename){
    std::string warn, err;
    std::vector<tinydng::DNGImage> images;

    // List of custom field infos. This is optional and can be empty.
    std::vector<tinydng::FieldInfo> custom_field_lists;

    // Loads all images(IFD) in the DNG file to `images` array.
    // You can use `LoadDNGFromMemory` API to load DNG image from a memory.
    bool ret = tinydng::LoadDNG(filename.c_str(), custom_field_lists, &images, &warn, &err);


    if (!warn.empty()) {
        std::cout << "Warn: " << warn << std::endl;
    }

    if (!err.empty()) {
        std::cerr << "Err: " << err << std::endl;
    }

    if (ret) {
        for (size_t i = 0; i < images.size(); i++) {
            const tinydng::DNGImage &image = images[i];

            std::cout << "width = " << image.width << std::endl;
            std::cout << "height = " << image.height << std::endl;
            std::cout << "bits per pixel = " << image.bits_per_sample << std::endl;
            std::cout << "bits per pixel(original) = " << image.bits_per_sample_original << std::endl;
            std::cout << "samples per pixel = " << image.samples_per_pixel << std::endl;
            std::cout << "planar config = " << image.planar_configuration << std::endl;
            std::cout << "orientation = " << image.orientation << std::endl;

        }
    }
    return images;
}

int main(int argc, char **argv) {
    std::string input_filename = "IMG_20231030_223215.dng";
    if (argc > 1) {
        input_filename = std::string(argv[1]);
    }

    auto images = readDNG(input_filename);
    tinydng::DNGImage image = images[0];

    auto data = new char[images[0].width * images[0].height];
    auto data2 = (uint16_t*)images[0].data.data();
    for(int i =0; i<images[0].width * images[0].height;i++){
        data[i] = char(data2[i]/((image.white_level[0]+1)/256));
    }
    stbi_write_png("test.png", images[0].width, images[0].height, 1, data, images[0].width); //PNG representation of bayer image

    tinydngwriter::DNGImage dng_image0;
    dng_image0.SetSubfileType(false,false,false);
    dng_image0.SetOrientation(image.orientation);
    dng_image0.SetImageWidth(image.width);
    dng_image0.SetImageLength(image.height);
    dng_image0.SetRowsPerStrip(1);
    dng_image0.SetActiveArea(reinterpret_cast<const unsigned int *>(image.active_area));
    dng_image0.SetResolutionUnit(tinydngwriter::RESUNIT_CENTIMETER);
    dng_image0.SetXResolution(72.f);
    dng_image0.SetYResolution(72.f);


    dng_image0.SetSamplesPerPixel(1);
    uint16_t bps[1] = {16};
    dng_image0.SetBitsPerSample(1, reinterpret_cast<const unsigned short *>(&bps));

    dng_image0.SetPhotometric(tinydngwriter::PHOTOMETRIC_CFA);
    dng_image0.SetPlanarConfig(tinydngwriter::PLANARCONFIG_CONTIG);
    dng_image0.SetCompression(tinydngwriter::COMPRESSION_NONE);
    uint16_t sampleFormat = tinydngwriter::SAMPLEFORMAT_UINT;
    dng_image0.SetSampleFormat(1, &sampleFormat);
    dng_image0.SetDNGVersion(0x5,0x2,0x0,0x1); // DNG version have reversed order for some reason
    dng_image0.SetBlackLevelRepeatDim(2,2);
    unsigned short bl[4] = {0,0,0,0};
    dng_image0.SetBlackLevel(4, bl);
    dng_image0.SetCFARepeatPatternDim(2, 2);

    unsigned char cfa[4] = {2,1,1,0}; //Set CFA manually
    dng_image0.SetCFAPattern(4,cfa);
    double wl = image.white_level[0];
    dng_image0.SetWhiteLevelRational(1,&wl);
    dng_image0.SetImageData(image.data.data(), image.data.size());

    dng_image0.SetAsShotNeutral(3, image.as_shot_neutral);
    //dng_image0.SetBlackLevel(4, reinterpret_cast<const unsigned short *>(image.black_level));
    dng_image0.SetBigEndian(true);
    dng_image0.SetCalibrationIlluminant1(image.calibration_illuminant1);
    dng_image0.SetCalibrationIlluminant2(image.calibration_illuminant2);
    dng_image0.SetColorMatrix1(9, reinterpret_cast<const double *>(image.color_matrix1));
    dng_image0.SetColorMatrix2(9, reinterpret_cast<const double *>(image.color_matrix2));
    dng_image0.SetForwardMatrix1(9, reinterpret_cast<const double *>(image.forward_matrix1));
    dng_image0.SetForwardMatrix2(9, reinterpret_cast<const double *>(image.forward_matrix2));
    dng_image0.SetCameraCalibration1(9, reinterpret_cast<const double *>(image.camera_calibration1));
    dng_image0.SetCameraCalibration2(9, reinterpret_cast<const double *>(image.camera_calibration2));
    tinydngwriter::DNGWriter dng_writer(true);
    dng_writer.AddImage(&dng_image0);

    std::string err;
    dng_writer.WriteToFile("testWrite2.dng", &err);

    if (!err.empty()) {
        std::cout << "Err: " << err << std::endl;
    }


    readDNG(std::string("testWrite2.dng")); //Trying to re-read the file

    return EXIT_SUCCESS;
}
