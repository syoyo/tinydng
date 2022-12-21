import tinydng

# Returns DNGImage[](array of images)
images = tinydng.loaddng("../../colorchart.dng")

print(len(images))

for img in images:
    print("width:", img.width)
    print("height:", img.height)
    print("bits per sample:", img.bits_per_sample)
