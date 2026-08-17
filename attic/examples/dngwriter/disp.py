import imageio

a = imageio.imread("output-fp32-grayscale.tiff")
print(a)

a = imageio.imread("test-32bit.tiff")
print(a)
