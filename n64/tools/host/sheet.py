import sys
from PIL import Image
names=sys.argv[2:]
ims=[Image.open(f'out/{n}.ppm') for n in names]
cols=min(4,len(ims)); rows=(len(ims)+cols-1)//cols
W=Image.new('RGB',(320*cols,240*rows))
for i,im in enumerate(ims): W.paste(im,((i%cols)*320,(i//cols)*240))
W.save(sys.argv[1])
