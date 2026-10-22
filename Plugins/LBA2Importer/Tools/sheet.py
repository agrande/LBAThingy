"""Contact sheet of 640x360 shots, two per row: sheet.py out.png file1 label1 file2 label2 ..."""
import sys
from PIL import Image, ImageDraw
out=sys.argv[1]; items=sys.argv[2:]
pairs=[(items[i],items[i+1]) for i in range(0,len(items),2)]
ims=[]
for f,l in pairs:
    im=Image.open(f).convert('RGB').resize((640,360),Image.LANCZOS)
    d=ImageDraw.Draw(im); d.rectangle((0,0,200,18),fill=(0,0,0)); d.text((4,3),l,fill=(255,255,255))
    ims.append(im)
cols=2; rows=(len(ims)+1)//2
sheet=Image.new('RGB',(640*cols,360*rows))
for i,im in enumerate(ims): sheet.paste(im,((i%cols)*640,(i//cols)*360))
sheet.save(out)
