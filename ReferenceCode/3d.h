/*    y|
       |   x
       | /
       |/_____z
                         */
       
#include "..\drivers\mvAltiVec.h"

#define DEGREE_CONV (180/M_PI)
#define HORIZONTAL  640
#define VERTICAL    480

typedef struct _vertex
{
    float           textureX;
    float           textureY;
    float           pos[3];
    unsigned int    numAdjPoly;
    unsigned int *  pAdjPoly;
    unsigned int    numAdjVert;
    unsigned int *  pAdjVert;
    float           n[3];
} VERTEX;

typedef struct _polygon
{
    unsigned int    vertexID[3];
    unsigned int    AdjPoly[3];
    float           n[3];
    unsigned int    R;
    unsigned int    G;
    unsigned int    B;
} POLYGON;

typedef struct _object
{
    float           pos[3];
    VERTEX *        pVertex;
    unsigned int    numVeritces;
    POLYGON *       pPolygon;
    unsigned int    numPolygons;
    unsigned int    status;     /* BIT0=seen BIT1=No shadow BIT2=floor BIT3=doublesided*/
    unsigned int    min;
    unsigned int    max;
    struct _object *        prevObj;
} OBJECT;

typedef struct _finalObject
{ 
    int             seenNumPoly;
    OBJECT *        pCurrentObj;
    int             polyIdList[1000];
    VERTEX          vertexList[1000];
    float           viewingXNormal[1000];
    int             minimalXDistance[1000];
    int             projX[1000];
    int             projY[1000];
} FINALOBJECT;

typedef struct _camera
{
    float           pos[3];
    float           direction[3];
    int             heading;         /* angle from x axis in xy plane - heading */ 
    int             pitch;        /* angle from xy plane and the view vector (z direction)- pitch */
    int             tilt;       /* tilt around x axis - bank*/
} CAMERA;

typedef struct _light
{
    float           pos[3];
    float           direction[3];
    int             heading;         /* angle from x axis in xy plane - heading */ 
    int             pitch;        /* angle from xy plane and the view vector (z direction)- pitch */
    int             tilt;       /* tilt around x axis - bank*/
    int             R;
    int             G;
    int             B;
} LIGHT;

#define gsin(x) ((x < 0)? sinAr[360+x]:sinAr[x])
#define gcos(x) ((x < 0)? cosAr[360+x]:cosAr[x])

#define REFRESH_SCREEN()  gtDmaTransfer(DMA_ENG_0, VIRTUAL_TO_PHY(frameBuffer),0x90000000,2048*VERTICAL,\
                                        DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES | DMA_DEST_ADDR_IN_PCI1,NULL)
                                    
//#define color(r, g, b)  ((r * 6 * 6) + (g * 6) + b)
#define color(r, g, b)  (r)

void init3d();

void matrixMul(MV_MATRIX_4X4 * m1, MV_MATRIX_4X4 * m2);

void vectorMatrixMul(float vec[], MV_MATRIX_4X4 * mat);

void scale( MV_MATRIX_4X4 *  mat, float sx, float sy, float sz);

void move( MV_MATRIX_4X4 *  mat, float dx, float dy, float dz);

void rotate(MV_MATRIX_4X4 *  mat, int x, int y, int z);
                                                                 
void rotateArbitrary(MV_MATRIX_4X4 *  mat, float basex, float basey, float basez, 
                     int xAxis, int yAxis, int zAxis, int alpha);

void scaleArbitrary(MV_MATRIX_4X4 *  mat, float basex, float basey, float basez, 
                     float xAxis, float yAxis, float zAxis);

void transformObject(OBJECT * pObject,MV_MATRIX_4X4 * mat);

void transformCamera(float dx, float dy, float dz, int dHeading, int dPitch, int dTilt);

void transformLight(float dx, float dy, float dz, int dHeading, int dPitch, int dTilt, int R, int G, int B);

int viewTransformation(OBJECT * pObject);

void sortObjects(OBJECT * obj, int numObjects);

void sortPolygon();

void projection();

int loadObjects(OBJECT ** object, char * fileName, int * numObjects);

void wireframe();

void flatShade(int shadowFlag);

void gouraudShade();

void copyObject(OBJECT ** dest, OBJECT * source, int * numObjects);

void fog(float density);

void lightDisplay();

void colorObject(OBJECT * pObj, int R, int G, int B);


#define BITMAPFILEHEADERSIZE        14
#define BITMAPINFOHEADERSIZE        40

typedef struct _bitMapFileHeader
{
    char            signature[2];   /* 'BM' */                                           
    unsigned int    fileSize;       /* File size in bytes */
    unsigned int    reserved;       
    unsigned int    dataOffset;     /* File offset to raster data */
} BITMAPFILEHEAD;

typedef struct _bitMapInfoHeader
{
    unsigned int    size;           /* Size of info header = 40 */
    unsigned int    width;          /* Bitmap width */
    unsigned int    height;         /* Bitmap height */
    short           planes;         /* Number of planes (=1) */
    short           bitCount;       /* Bits per pixel */
                                    /* 1 = monochrome palette, NumColors = 1 */
                                    /* 4 = 4bit palletized, NumColors = 16 */
                                    /* 8 = 8bit palletized, NumColors = 256 */
                                    /* 16 = 16bit RGB, NumColors = 65536 */
                                    /* 24 = 24bit RGB, NumColors = 16M */
    unsigned int    compression;    /* Type of compression */
                                    /* 0 = BI_RGB - No compression */
                                    /* 1 = BI_RLE8 - 8bit RLE encoding */
                                    /* 2 = BI_RLE4 - 4bit RLE encoding */
    unsigned int    imageSize;      /* (compressed) Size of image */
                                    /* It is valid to set this =0
                                       if compression = 0 */
    unsigned int    xPixelsPerM;    /* Horizontal resolution: Pixels/meter */
    unsigned int    yPixelsPerM;    /* Vertical resolution: Pixels/meter */
    unsigned int    colorsUsed;     /* Number of actually used colors */
    unsigned int    colorsImportant;/* Number of important colors, 0 = all */
    unsigned char * colorTable;     /* Present only if info.bitCount <= 8 */
                                    /* size = 4*NumColors bytes */
                                    /* Red - 1 byte */
                                    /* Green - 1 byte */
                                    /* Blue - 1 byte */
                                    /* reserved - 1 byte (=0) */
} BITMAPINFOHEAD;

typedef struct _bitmap
{
    BITMAPFILEHEAD *    bitMapFileHeaderPointer;
    BITMAPINFOHEAD *    bitMapInfoHeaderPointer;
    unsigned char *     data;
} BMP;
