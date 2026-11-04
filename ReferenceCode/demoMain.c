
#include "3d.h"
#include "..\drivers\ev64360.h"
#include "..\drivers\gtSysGal.h"
#include "..\drivers\gtFS.h"
#include "..\drivers\gtFlash.h"
#include "..\drivers\gtDma.h"
#include "..\drivers\gtPci.h"
#include "..\drivers\gtMemory.h"
#include "..\drivers\cirrus.h"
#include "..\drivers\mvAltiVec.h"
#include "..\drivers\gtAuxil.h"
#include "..\drivers\gtCntmr.h"
#include <math.h>


extern float* tempVec3;
extern float cosAr[360], sinAr[360];
extern unsigned char * frameBuffer;
extern  int * zBuffer;
extern CAMERA camera;
extern int objIdList[200];
extern LIGHT light;
int marvellNumObj=0,motorolaNumObj=0,ballNumObj=0,floorNumObj=0,lettersNumObj=0,numbersNumObj=0;
int RUReadyNumObj=0,cNumberNumObj=0, atlantisNumObj=0;


MV_MATRIX_4X4 *mat = 0x84000000;
MV_MATRIX_4X4 *mat1= 0x84000100;
MV_MATRIX_4X4 *mat2= 0x84000200;
MV_MATRIX_4X4 *t1  = 0x84000300;
MV_MATRIX_4X4 *t2  = 0x84000400;
MV_MATRIX_4X4 *t3  = 0x84000500;
MV_MATRIX_4X4 *t4  = 0x84000600;
MV_MATRIX_4X4 *t5  = 0x84000700;
MV_MATRIX_4X4 *t6  = 0x84000800;
float   * tempVec  = 0x84000900;
float   * tempVec1 = 0x84000920;
float   * tempVec2 = 0x84000940;
//float   * tempVec3 = 0x83000960;
//float   * tempVec4 = 0x83000980;

void main()
{
    unsigned int temp;
    int i,j,k,l, flag, bflag[6], marvellAngle=0, motRingAngle=0;
    float t[6],v[6],t0[6],v0[6],x[6],z[6],alpha[6],mx,my,mz,vec[3];
    unsigned int    mvTclock;
    double          time;

    OBJECT * marvell;
    OBJECT * motorola;
    OBJECT * ball;
    OBJECT * floor;
    OBJECT * letters;
    OBJECT * numbers;
    OBJECT * RUReady;
    OBJECT * cNumber;
    OBJECT * atlantis;
    BMP  bubba;

    void * voidPointer;
    
    ENABLE_ALTIVEC();
    // Initialize V0 = {0,0,0,0}
    mat->m[0][0] = 0;
    mat->m[0][1] = 0;
    mat->m[0][2] = 0;
    mat->m[0][3] = 0;
    MV_AV_LOAD_VEC(0,mat->m[0]);

    printf("Extracting Tclock...");
    mvTclock = gtAuxilExtractTclock(CNTMR_0);
    printf("%d Hz\n",mvTclock);

 //   printf("Waiting for CPU 1\n");
  //  while(GT_READWORD(0xf2400000) != 0x123456);
 //   GT_WRITE_WORD(0xf2400000, 0x0);
    
    printf("Start the show\n");
    printf ("flash=%d\n",gtFlashInit(gtMemoryGetDeviceWindowBaseAddr(DEVICE3) ,4,X16));
  //  printf ("flash=%d\n",gtFlashInit(gtMemoryGetDeviceBaseAddress(DEVICE3) ,4,X16));
    i = gtFSInit(FS_NO_CACHE);
        
    /* Master enable */
    GT_PCI_1_MASTER_ENABLE(PCI_SELF);
    GT_PCI_1_SLAVE_ENABLE(7);
    GT_PCI_1_IO_ENABLE(7);
    GT_PCI_1_IO_ENABLE(PCI_SELF);
    
    GT_SET_REG_BITS( 0x1424, BIT24);
    GT_SET_REG_BITS( 0xa18, BIT13);
    GT_RESET_REG_BITS( 0xa18, BIT12);

    /* Video card initialization */
    printf("Enable the Cirrus logic...\n");
    gtPci1WriteConfigReg(0x14, 7, 0x10000000);      //If there is a second BAR we move it.
    enableCirrusVgaCard(640,480,24,0x90000000,1);
    printf("pci=%x\n",gtPci1ReadConfigReg(0x10,7));
    printf("enable ext. mem\n");
    cirrusEnableExtendedMemory(); 


    printf("Bitmap loading\n");
    cBitMap("bubbabmp",&bubba); 
 /*   for(i = 0 ; i < 640*480*3 ; i+=1)
        GT_WRITE_WORD(VIRTUAL_TO_PHY((unsigned int)bubba.data)+i, GT_READWORD_CACHEABLE((unsigned int)bubba.data + i));
    bubba.data = VIRTUAL_TO_PHY(bubba.data);
 */   printf("bubbabmp=%x bit=%d width=%d height=%d\n",bubba.data,bubba.bitMapInfoHeaderPointer->bitCount,bubba.bitMapInfoHeaderPointer->width,bubba.bitMapInfoHeaderPointer->height);
    init3d();
    /* Memory init */
    for(k = 0 ; k < 0x100 ; k++)
    {
        GT_WRITE_WORD(4*k + 0x1f00100, 0xfffffff);
    }

    /* Light initialization */
    light.pos[0] = -5850;
    light.pos[1] = 950;
    light.pos[2] = 600;
    light.R = 240;
    light.G = 240;
    light.B = 200;
    light.heading = 0;
    light.pitch = 0;
    light.tilt = 0;

    transformCamera(-5000,700,100,0,-5,0);

    /*Object loading*/
    printf("load objects\n");
    printf("loading marvell\n");
    if(loadObjects(&marvell,"marvell3d",&marvellNumObj)==0)
        return;
    printf("loading motorola\n");
    if(loadObjects(&motorola,"motorola3d",&motorolaNumObj)==0)
        return;
    printf("loading ball\n");
    if(loadObjects(&ball,"ball3d",&ballNumObj)==0)
        return; 
    printf("loading floor objects\n");
    if(loadObjects(&floor,"floor3d",&floorNumObj)==0)
        return;
    printf("loading letters objects\n");
    if(loadObjects(&letters,"letters3d",&lettersNumObj)==0)
        return;
    printf("loading numbers objects\n");
    if(loadObjects(&numbers,"numbers3d",&numbersNumObj)==0)
        return;
    printf("marv=%d mot=%d ball=%d floor=%d lett=%d num=%d\n",marvellNumObj,motorolaNumObj,
           ballNumObj,floorNumObj,lettersNumObj,numbersNumObj);
    /*creating Marvell object */
    copyObject(&marvell, &marvell[2],&marvellNumObj);
    copyObject(&marvell, &marvell[2],&marvellNumObj);
    copyObject(&marvell, &marvell[0],&marvellNumObj);
    copyObject(&marvell, &marvell[1],&marvellNumObj);
    copyObject(&marvell, &marvell[2],&marvellNumObj);
    copyObject(&marvell, &marvell[2],&marvellNumObj);
    copyObject(&marvell, &marvell[2],&marvellNumObj);

    move(mat, 0, 0, 100);
    transformObject(&marvell[5],mat);
    move(mat, 0, 0, 100);
    transformObject(&marvell[6],mat);
    move(mat, 0, -15, 0);
    transformObject(&marvell[3],mat);
    move(mat, 0, -30, 0);
    transformObject(&marvell[4],mat);
    move(mat, 0, 0, 100);
    transformObject(&marvell[7],mat);
    move(mat, 0, -15, 100);
    transformObject(&marvell[8],mat);
    move(mat, 0, -30, 100);
    transformObject(&marvell[9],mat);

    /* Balls creation */
    copyObject(&ball, &ball[0],&ballNumObj);
    copyObject(&ball, &ball[0],&ballNumObj);
    copyObject(&ball, &ball[0],&ballNumObj);
    copyObject(&ball, &ball[0],&ballNumObj);
    move(mat,300, 750, 350);
    transformObject(&ball[0], mat);
    move(mat,50, 650, -80);
    transformObject(&ball[1], mat);
    move(mat,100, 700, 450);
    transformObject(&ball[2], mat);
    move(mat,-100, 600, 200);
    transformObject(&ball[3], mat);
    move(mat,400, 800, 0);
    transformObject(&ball[4], mat);

    /* initializing floor  coordinates */
    //colorObject(&floor[0], 180, 180, 100);
   // rotateArbitrary(mat1, floor[0].pos[0], floor[0].pos[1],floor[0].pos[2],0,0,10,-90);
    move(mat,-250, 0, -500);
   // matrixMul(mat, mat1);
    transformObject(&floor[0], mat);

    /* Marvell starting positions */
    move(mat, 720, 720 +250, 720);
    transformObject(&marvell[0],mat);
    move(mat, 720, -720+250, -720);
    transformObject(&marvell[1],mat);
    move(mat, 0, 0+250, 720);
    transformObject(&marvell[2],mat);
    move(mat, 0, -720+250, -720);
    transformObject(&marvell[3],mat);
    move(mat, 0, -720+250, 720);
    transformObject(&marvell[4],mat);
    move(mat, 0, 720+250, -720);
    transformObject(&marvell[5],mat);
    move(mat, 0, 720+250, 720);
    transformObject(&marvell[6],mat);
    move(mat, 720, -720+250, 720);
    transformObject(&marvell[7],mat);
    move(mat, 720, 720+250, -720);
    transformObject(&marvell[8],mat);
    move(mat, -720, 0+250, 0);
    transformObject(&marvell[9],mat);

    /* Motoroloa init */
    scaleArbitrary(mat,motorola[0].pos[0],motorola[0].pos[1],motorola[0].pos[2], 0.8,0.8,0.8);
    move(mat1,80, -220, 100);
    matrixMul(mat1, mat);
    transformObject(&motorola[0], mat1);
    scaleArbitrary(mat,motorola[1].pos[0],motorola[1].pos[1],motorola[1].pos[2], 0.8,0.8,0.8);
    move(mat1,-880, 190, 100);
    matrixMul(mat1, mat);
    transformObject(&motorola[1], mat1);
    printf("mot0 x=%f y=%f z=%f\n",motorola[0].pos[0],motorola[0].pos[1],motorola[0].pos[2]);
    printf("mot1 x=%f y=%f z=%f\n",motorola[1].pos[0],motorola[1].pos[1],motorola[1].pos[2]);

    /* Create words - are you ready*/
    printf("RUReady create\n");
    copyObject(&RUReady, &letters[0],&RUReadyNumObj);      //A
    copyObject(&RUReady, &letters[17],&RUReadyNumObj);     //R
    copyObject(&RUReady, &letters[4],&RUReadyNumObj);      //E
    copyObject(&RUReady, &letters[24],&RUReadyNumObj);     //Y
    copyObject(&RUReady, &letters[14],&RUReadyNumObj);     //O
    copyObject(&RUReady, &letters[20],&RUReadyNumObj);     //U
    copyObject(&RUReady, &letters[17],&RUReadyNumObj);     //R
    copyObject(&RUReady, &letters[4],&RUReadyNumObj);      //E
    copyObject(&RUReady, &letters[0],&RUReadyNumObj);      //A
    copyObject(&RUReady, &letters[3],&RUReadyNumObj);      //D
    copyObject(&RUReady, &letters[24],&RUReadyNumObj);     //Y
    printf("RUReady inti=%d\n",RUReadyNumObj);
    move(mat, -5200, 670, 0+50);
    transformObject(&RUReady[0], mat);
    move(mat, -5200, 670, 40+50);
    transformObject(&RUReady[1], mat);
    move(mat, -5200, 670, 80+50);
    transformObject(&RUReady[2], mat);
    move(mat, -5200, 670, 0+50);
    transformObject(&RUReady[3], mat);
    move(mat, -5200, 670, 40+50);
    transformObject(&RUReady[4], mat);
    move(mat, -5200, 670, 80+50);
    transformObject(&RUReady[5], mat);
    move(mat, -5200, 670, 0+20);
    transformObject(&RUReady[6], mat);
    move(mat, -5200, 670, 40+20);
    transformObject(&RUReady[7], mat);
    move(mat, -5200, 670, 80+20);
    transformObject(&RUReady[8], mat);
    move(mat, -5200, 670, 120+20);
    transformObject(&RUReady[9], mat);
    move(mat, -5200, 670, 160+20);
    transformObject(&RUReady[10], mat);
    move(mat, -5200, 670, 200+20);

    printf("create atlantis text\n");
    /* Create words - atlantis spec */
    copyObject(&atlantis, &numbers[1],&atlantisNumObj);      //1     0
    copyObject(&atlantis, &numbers[3],&atlantisNumObj);      //3
    copyObject(&atlantis, &numbers[3],&atlantisNumObj);      //3
    copyObject(&atlantis, &letters[12],&atlantisNumObj);     //M
    copyObject(&atlantis, &letters[7],&atlantisNumObj);      //H
    copyObject(&atlantis, &letters[25],&atlantisNumObj);     //Z     5
    move(mat, -250, 1000, 0-50);
    transformObject(&atlantis[0], mat);
    move(mat, -250, 1000, 40-50);
    transformObject(&atlantis[1], mat);
    move(mat, -250, 1000, 80-50);
    transformObject(&atlantis[2], mat);
    move(mat, -250, 1000, 120-50);
    transformObject(&atlantis[3], mat);
    move(mat, -250, 1000, 160-50);
    transformObject(&atlantis[4], mat);
    move(mat, -250, 1000, 200-50);
    transformObject(&atlantis[5], mat);
    copyObject(&atlantis, &numbers[4],&atlantisNumObj);     //4      6
    copyObject(&atlantis, &letters[3],&atlantisNumObj);     //D
    copyObject(&atlantis, &letters[12],&atlantisNumObj);    //M
    copyObject(&atlantis, &letters[0],&atlantisNumObj);     //A
    copyObject(&atlantis, &letters[2],&atlantisNumObj);     //C
    copyObject(&atlantis, &letters[7],&atlantisNumObj);     //H
    copyObject(&atlantis, &letters[0],&atlantisNumObj);     //A
    copyObject(&atlantis, &letters[13],&atlantisNumObj);    //N
    copyObject(&atlantis, &letters[13],&atlantisNumObj);    //N
    copyObject(&atlantis, &letters[4],&atlantisNumObj);     //E
    copyObject(&atlantis, &letters[11],&atlantisNumObj);    //L
    copyObject(&atlantis, &letters[18],&atlantisNumObj);    //S      17
    move(mat, -250, 1000, 0-50);
    transformObject(&atlantis[6], mat);
    move(mat, -250, 1000, 80-50);
    transformObject(&atlantis[7], mat);
    move(mat, -250, 1000, 120-50);
    transformObject(&atlantis[8], mat);
    move(mat, -250, 1000, 160-50);
    transformObject(&atlantis[9], mat);
    move(mat, -250, 1000, 240-50);
    transformObject(&atlantis[10], mat);
    move(mat, -250, 1000, 280-50);
    transformObject(&atlantis[11], mat);
    move(mat, -250, 1000, 320-50);
    transformObject(&atlantis[12], mat);
    move(mat, -250, 1000, 360-50);
    transformObject(&atlantis[13], mat);
    move(mat, -250, 1000, 400-50);
    transformObject(&atlantis[14], mat);
    move(mat, -250, 1000, 440-50);
    transformObject(&atlantis[15], mat);
    move(mat, -250, 1000, 480-50);
    transformObject(&atlantis[16], mat);
    move(mat, -250, 1000, 520-50);
    transformObject(&atlantis[17], mat);
    for(i = 6 ; i <= 17 ; i++)
    {
        colorObject(&atlantis[i],240,240,0);
        rotateArbitrary(mat,800, 1000, 200, 0, 10, 0, -20);
        transformObject(&atlantis[i], mat);
    }
    copyObject(&atlantis, &letters[3],&atlantisNumObj);     //D      18
    copyObject(&atlantis, &letters[3],&atlantisNumObj);     //D
    copyObject(&atlantis, &letters[17],&atlantisNumObj);    //R
    copyObject(&atlantis, &letters[18],&atlantisNumObj);    //S
    copyObject(&atlantis, &letters[3],&atlantisNumObj);     //D
    copyObject(&atlantis, &letters[17],&atlantisNumObj);    //R
    copyObject(&atlantis, &letters[0],&atlantisNumObj);     //A
    copyObject(&atlantis, &letters[12],&atlantisNumObj);    //M      25
    move(mat, -250, 1000, 0-50);
    transformObject(&atlantis[18], mat);
    move(mat, -250, 1000, 40-50);
    transformObject(&atlantis[19], mat);
    move(mat, -250, 1000, 80-50);
    transformObject(&atlantis[20], mat);
    move(mat, -250, 1000, 160-50);
    transformObject(&atlantis[21], mat);
    move(mat, -250, 1000, 200-50);
    transformObject(&atlantis[22], mat);
    move(mat, -250, 1000, 240-50);
    transformObject(&atlantis[23], mat);
    move(mat, -250, 1000, 280-50);
    transformObject(&atlantis[24], mat);
    move(mat, -250, 1000, 320-50);
    transformObject(&atlantis[25], mat);
    for(i = 18 ; i <= 25 ; i++)
    {
        rotateArbitrary(mat,800, 1000, 200, 0, 10, 0, -55);
        transformObject(&atlantis[i], mat);
    }
    copyObject(&atlantis, &numbers[3],&atlantisNumObj);     //3      26
    copyObject(&atlantis, &letters[6],&atlantisNumObj);     //G
    copyObject(&atlantis, &letters[8],&atlantisNumObj);     //I
    copyObject(&atlantis, &letters[6],&atlantisNumObj);     //G
    copyObject(&atlantis, &letters[0],&atlantisNumObj);     //A
    copyObject(&atlantis, &letters[1],&atlantisNumObj);     //B
    copyObject(&atlantis, &letters[8],&atlantisNumObj);     //I
    copyObject(&atlantis, &letters[19],&atlantisNumObj);    //T      
    copyObject(&atlantis, &letters[15],&atlantisNumObj);    //P
    copyObject(&atlantis, &letters[14],&atlantisNumObj);    //O
    copyObject(&atlantis, &letters[17],&atlantisNumObj);    //R
    copyObject(&atlantis, &letters[19],&atlantisNumObj);    //T
    copyObject(&atlantis, &letters[18],&atlantisNumObj);    //S      38
    move(mat, -250, 1000, 0-50);
    transformObject(&atlantis[26], mat);
    move(mat, -250, 1000, 80-50);
    transformObject(&atlantis[27], mat);
    move(mat, -250, 1000, 120-50);
    transformObject(&atlantis[28], mat);
    move(mat, -250, 1000, 160-50);
    transformObject(&atlantis[29], mat);
    move(mat, -250, 1000, 200-50);
    transformObject(&atlantis[30], mat);
    move(mat, -250, 1000, 240-50);
    transformObject(&atlantis[31], mat);
    move(mat, -250, 1000, 280-50);
    transformObject(&atlantis[32], mat);
    move(mat, -250, 1000, 320-50);
    transformObject(&atlantis[33], mat);
    move(mat, -250, 1000, 400-50);
    transformObject(&atlantis[34], mat);
    move(mat, -250, 1000, 440-50);
    transformObject(&atlantis[35], mat);
    move(mat, -250, 1000, 480-50);
    transformObject(&atlantis[36], mat);
    move(mat, -250, 1000, 520-50);
    transformObject(&atlantis[37], mat);
    move(mat, -250, 1000, 560-50);
    transformObject(&atlantis[38], mat);
    for(i = 26 ; i <= 38 ; i++)
    {
        colorObject(&atlantis[i],240,240,0);
        rotateArbitrary(mat,800, 1000, 200, 0, 10, 0, -80);
        transformObject(&atlantis[i], mat);
    }

    copyObject(&atlantis, &letters[15],&atlantisNumObj);    //P      39
    copyObject(&atlantis, &letters[2],&atlantisNumObj);     //C
    copyObject(&atlantis, &letters[8],&atlantisNumObj);     //I
    copyObject(&atlantis, &letters[23],&atlantisNumObj);    //X      
    copyObject(&atlantis, &numbers[1],&atlantisNumObj);     //1      
    copyObject(&atlantis, &numbers[3],&atlantisNumObj);     //3      
    copyObject(&atlantis, &numbers[3],&atlantisNumObj);     //3      45
    move(mat, -250, 1000, 0-50);
    transformObject(&atlantis[39], mat);
    move(mat, -250, 1000, 40-50);
    transformObject(&atlantis[40], mat);
    move(mat, -250, 1000, 80-50);
    transformObject(&atlantis[41], mat);
    move(mat, -250, 1000, 150-50);
    transformObject(&atlantis[42], mat);
    move(mat, -250, 1000, 200-50);
    transformObject(&atlantis[43], mat);
    move(mat, -250, 1000, 240-50);
    transformObject(&atlantis[44], mat);
    move(mat, -250, 1000, 280-50);
    transformObject(&atlantis[45], mat);
    for(i = 39 ; i <= 45 ; i++)
    {
        rotateArbitrary(mat,800, 1000, 200, 0, 10, 0, -120);
        transformObject(&atlantis[i], mat);
    }

    copyObject(&atlantis, &letters[3],&atlantisNumObj);     //D      46
    copyObject(&atlantis, &letters[8],&atlantisNumObj);     //I
    copyObject(&atlantis, &letters[18],&atlantisNumObj);    //S
    copyObject(&atlantis, &letters[2],&atlantisNumObj);     //C
    copyObject(&atlantis, &letters[14],&atlantisNumObj);    //O
    copyObject(&atlantis, &letters[21],&atlantisNumObj);    //V
    copyObject(&atlantis, &letters[4],&atlantisNumObj);     //E
    copyObject(&atlantis, &letters[17],&atlantisNumObj);    //R      
    copyObject(&atlantis, &letters[24],&atlantisNumObj);    //Y
    copyObject(&atlantis, &letters[8],&atlantisNumObj);     //I
    copyObject(&atlantis, &letters[8],&atlantisNumObj);     //I      56
    move(mat, -250, 1000, 0-50);
    transformObject(&atlantis[46], mat);
    move(mat, -250, 1000, 40-50);
    transformObject(&atlantis[47], mat);
    move(mat, -250, 1000, 80-50);
    transformObject(&atlantis[48], mat);
    move(mat, -250, 1000, 120-50);
    transformObject(&atlantis[49], mat);
    move(mat, -250, 1000, 160-50);
    transformObject(&atlantis[50], mat);
    move(mat, -250, 1000, 200-50);
    transformObject(&atlantis[51], mat);
    move(mat, -250, 1000, 240-50);
    transformObject(&atlantis[52], mat);
    move(mat, -250, 1000, 280-50);
    transformObject(&atlantis[53], mat);
    move(mat, -250, 1000, 320-50);
    transformObject(&atlantis[54], mat);
    move(mat, -250, 1000, 400-50);
    transformObject(&atlantis[55], mat);
    move(mat, -250, 1000, 440-50);
    transformObject(&atlantis[56], mat);
    for(i = 46 ; i <= 56 ; i++)
    {
        colorObject(&atlantis[i],240,240,0);
        rotateArbitrary(mat,800, 1000, 200, 0, 10, 0, 35);
        transformObject(&atlantis[i], mat);
    }
    /*
    copyObject(&atlantis, &letters[8],&atlantisNumObj);     //I      57
    copyObject(&atlantis, &letters[13],&atlantisNumObj);    //N
    copyObject(&atlantis, &letters[19],&atlantisNumObj);    //T
    copyObject(&atlantis, &letters[4],&atlantisNumObj);     //E
    copyObject(&atlantis, &letters[17],&atlantisNumObj);    //R
    copyObject(&atlantis, &letters[13],&atlantisNumObj);    //N
    copyObject(&atlantis, &letters[0],&atlantisNumObj);     //A
    copyObject(&atlantis, &letters[11],&atlantisNumObj);    //L      
    copyObject(&atlantis, &letters[18],&atlantisNumObj);    //S
    copyObject(&atlantis, &letters[17],&atlantisNumObj);    //R
    copyObject(&atlantis, &letters[0],&atlantisNumObj);     //A      
    copyObject(&atlantis, &letters[12],&atlantisNumObj);    //M      68
    move(mat, -250, 1000, 0-50);
    transformObject(&atlantis[57], mat);
    move(mat, -250, 1000, 40-50);
    transformObject(&atlantis[58], mat);
    move(mat, -250, 1000, 80-50);
    transformObject(&atlantis[59], mat);
    move(mat, -250, 1000, 120-50);
    transformObject(&atlantis[60], mat);
    move(mat, -250, 1000, 160-50);
    transformObject(&atlantis[61], mat);
    move(mat, -250, 1000, 200-50);
    transformObject(&atlantis[62], mat);
    move(mat, -250, 1000, 240-50);
    transformObject(&atlantis[63], mat);
    move(mat, -250, 1000, 280-50);
    transformObject(&atlantis[64], mat);
    move(mat, -250, 1000, 360-50);
    transformObject(&atlantis[65], mat);
    move(mat, -250, 1000, 400-50);
    transformObject(&atlantis[66], mat);
    move(mat, -250, 1000, 440-50);
    transformObject(&atlantis[67], mat);
    move(mat, -250, 1000, 480-50);
    transformObject(&atlantis[68], mat);
    for(i = 57 ; i <= 68 ; i++)
    {
        colorObject(&atlantis[i],240,240,0);
        rotateArbitrary(mat,800, 1000, 200, 0, 10, 0, -160);
        transformObject(&atlantis[i], mat);
    }       */

    /* Copy numbers */
    printf("create numbers and  init\n");
    copyObject(&cNumber, &numbers[0],&cNumberNumObj);
    asm("eieio");
    copyObject(&cNumber, &numbers[1],&cNumberNumObj);      
    asm("eieio");
    copyObject(&cNumber, &numbers[2],&cNumberNumObj);      
    asm("eieio");
    copyObject(&cNumber, &numbers[3],&cNumberNumObj);      
    asm("eieio");
    copyObject(&cNumber, &numbers[4],&cNumberNumObj);      
    asm("eieio");
    copyObject(&cNumber, &numbers[5],&cNumberNumObj);      
    asm("eieio");
    copyObject(&cNumber, &numbers[6],&cNumberNumObj);      
    asm("eieio");
    copyObject(&cNumber, &numbers[7],&cNumberNumObj);      
    asm("eieio");
    copyObject(&cNumber, &numbers[8],&cNumberNumObj);      
    asm("eieio");
    copyObject(&cNumber, &numbers[9],&cNumberNumObj);  
    printf("init numbers\n");
    colorObject(&cNumber[0],240,0,0);
    colorObject(&cNumber[1],0,240,0);
    colorObject(&cNumber[2],0,0,240);
    colorObject(&cNumber[3],240,0,240);
    colorObject(&cNumber[4],240,240,0);
    colorObject(&cNumber[5],0,240,240);
    colorObject(&cNumber[6],240,120,120);
    colorObject(&cNumber[7],120,240,120);
    colorObject(&cNumber[8],120,120,240);
    colorObject(&cNumber[9],240,240,240);
    move(mat, -5200, 670, 0+50);
    transformObject(&cNumber[0], mat);
    move(mat, -5200, 670, 0+100);
    transformObject(&cNumber[1], mat);
    move(mat, -5200, 670, 0+50);
    transformObject(&cNumber[2], mat);
    move(mat, -5200, 670, 0+100);
    transformObject(&cNumber[3], mat);
    move(mat, -5200, 670, 0+50);
    transformObject(&cNumber[4], mat);
    move(mat, -5200, 670, 0+100);
    transformObject(&cNumber[5], mat);
    move(mat, -5200, 670, 0+50);
    transformObject(&cNumber[6], mat);
    move(mat, -5200, 670, 0+100);
    transformObject(&cNumber[7], mat);
    move(mat, -5200, 670, 0+50);
    transformObject(&cNumber[8], mat);
    move(mat, -5200, 670, 0+100);
    transformObject(&cNumber[9], mat);
    move(mat, -5200, 670, 0+50);


    gtPci1WriteConfigReg(0x10,7,0x90000008);
    /* START THE SHOW */
    printf("show start\n");
    printf("Frame buffer's address = %x\n",frameBuffer);
    printf("pci=%x\n",gtPci1ReadConfigReg(0x10,7));
    while(1)
    {
        // Are you ready 
        while(RUReady[0].pos[0] < -2500)
        {
            // Initializing Frame Buffer
            for(i = 0 ; i < 480 ; i++)
            {
                while(gtDmaIsChannelActive(0));
                gtDmaTransfer(DMA_ENG_0,bubba.data + i * 1920 , VIRTUAL_TO_PHY(frameBuffer) + (i) * 2048, 1920,         //0x1a00000
                              DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES, NULL);
            }
            // Initializing Z Buffer
            gtDmaTransfer(DMA_ENG_2, 0x1f00100, VIRTUAL_TO_PHY(zBuffer) , 640*480*4,
                          DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES | DMA_HOLD_SOURCE_ADDR, NULL);
            for(k = 0 ; k < 3 ; k++)
            {               
                move(mat,20, 0, 0);
                rotateArbitrary(mat1, RUReady[1].pos[0], RUReady[1].pos[1], RUReady[1].pos[2],10, 0, 0, -10);
                matrixMul(mat, mat1);
                transformObject(&RUReady[k], mat);
                if(viewTransformation(&RUReady[k]) == 1)
                {
                    projection();
                    while(gtDmaIsChannelActive(DMA_ENG_2));
                    // Writing to frame buffer
                    gouraudShade();
                }
            }           
            REFRESH_SCREEN();
        }
        while(RUReady[3].pos[0] < -2500)
        {
            while(gtDmaIsChannelActive(DMA_ENG_0));
           // gtDmaTransfer(DMA_ENG_1, 0x1a00000, VIRTUAL_TO_PHY(frameBuffer) , 2048*480,
           //               DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES, NULL);
            for(i = 0 ; i < 480 ; i++)
            {
                while(gtDmaIsChannelActive(0));
                gtDmaTransfer(DMA_ENG_0,bubba.data + i * 1920 , VIRTUAL_TO_PHY(frameBuffer) + (i) * 2048, 1920,         //0x1a00000
                              DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES, NULL);
            }
            gtDmaTransfer(DMA_ENG_2, 0x1f00100, VIRTUAL_TO_PHY(zBuffer) , 640*480*4,
                          DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES | DMA_HOLD_SOURCE_ADDR, NULL);
            for(k = 3 ; k < 6 ; k++)
            {
                move(mat,20, 0, 0);
                rotateArbitrary(mat1, RUReady[4].pos[0], RUReady[4].pos[1], RUReady[4].pos[2],10, 0, 0, -10);
                matrixMul(mat, mat1);
                transformObject(&RUReady[k], mat);
                if(viewTransformation(&RUReady[k]) == 1)
                {
                    projection();
                    while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                    gouraudShade();
                }
            }
            REFRESH_SCREEN();
        }
        while(RUReady[6].pos[0] < -1500)
        {
            while(gtDmaIsChannelActive(DMA_ENG_0));
            for(i = 0 ; i < 480 ; i++)
            {
                while(gtDmaIsChannelActive(0));
                gtDmaTransfer(DMA_ENG_0,bubba.data + i * 1920 , VIRTUAL_TO_PHY(frameBuffer) + (i) * 2048, 1920,         //0x1a00000
                              DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES, NULL);
            }
            gtDmaTransfer(DMA_ENG_2, 0x1f00100, VIRTUAL_TO_PHY(zBuffer) , 640*480*4,
                          DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES | DMA_HOLD_SOURCE_ADDR, NULL);
            for(k = 6 ; k < 11 ; k++)
            {
                move(mat,20, 0, 0);
                rotateArbitrary(mat1, RUReady[8].pos[0], RUReady[8].pos[1], RUReady[8].pos[2],10, 0, 0, -10);
                matrixMul(mat, mat1);
                transformObject(&RUReady[k], mat);
                if(viewTransformation(&RUReady[k]) == 1)
                {
                    projection();
                    while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                    gouraudShade();
                }
            }
            REFRESH_SCREEN();
        }
 
        /* Countdown */
        while(cNumber[0].pos[0] < -2500)
        {
            while(gtDmaIsChannelActive(DMA_ENG_0));
            for(i = 0 ; i < 480 ; i++)
            {
                while(gtDmaIsChannelActive(0));
                gtDmaTransfer(DMA_ENG_0,bubba.data + i * 1920 , VIRTUAL_TO_PHY(frameBuffer) + (i) * 2048, 1920,         //0x1a00000
                              DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES, NULL);
            }
            gtDmaTransfer(DMA_ENG_2, 0x1f00100, VIRTUAL_TO_PHY(zBuffer) , 640*480*4,
                          DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES | DMA_HOLD_SOURCE_ADDR, NULL);
            for(k = 0 ; k < 10 ; k++)
            {
                if((k < 9 && cNumber[k+1].pos[0] > -4400) || k == 9)
                {
                    move(mat,20, 0, 0);
                    rotateArbitrary(mat1,cNumber[k].pos[0],cNumber[k].pos[1],cNumber[k].pos[2],0,10,0,10);
                    matrixMul(mat,mat1);
                    transformObject(&cNumber[k], mat);
                    if(viewTransformation(&cNumber[k]) == 1)
                    {
                        projection();
                        while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                        gouraudShade();
                    }
                }
            }
            REFRESH_SCREEN();
        }
    
        light.pos[0] = -850;
        light.pos[1] = 750;
        light.pos[2] = 400;
        /*floor approach */
        while(1)
        {
            if(camera.pos[0] < -700)
                transformCamera(20, -2, 0,0,0,6);
            else if(camera.tilt != 0)
                transformCamera(0, 0, 0,0,0,6);
            else
                break;
            while(gtDmaIsChannelActive(DMA_ENG_0));
            for(i = 0 ; i < 480 ; i++)
            {
                while(gtDmaIsChannelActive(0));
                gtDmaTransfer(DMA_ENG_0,bubba.data + i * 1920 , VIRTUAL_TO_PHY(frameBuffer) + (i) * 2048, 1920,         //0x1a00000
                              DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES, NULL);
            }
            gtDmaTransfer(DMA_ENG_2, 0x1f00100, VIRTUAL_TO_PHY(zBuffer) , 640*480*4,
                          DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES | DMA_HOLD_SOURCE_ADDR, NULL);
            if(viewTransformation(&floor[0]) == 1)
            {
                projection();
                while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                flatShade(0);
                //gouraudShade();
                //textureMap(&pbmp,0);
            }
            lightDisplay();
            REFRESH_SCREEN();
        }
    
        /* Balls fall*/
        flag = 0;
        for(k = 0 ; k < ballNumObj ; k++)
        {
            t0[k] = sqrt(0.2 * (ball[k].pos[1]));
            t[k] = 0;
            v0[k] = 0;
            v[k] = 0;
        }
        while(flag == 0)
        {
            flag = 1;
            while(gtDmaIsChannelActive(DMA_ENG_0));
            for(i = 0 ; i < 480 ; i++)
            {
                while(gtDmaIsChannelActive(0));
                gtDmaTransfer(DMA_ENG_0,bubba.data + i * 1920 , VIRTUAL_TO_PHY(frameBuffer) + (i) * 2048, 1920,         //0x1a00000
                              DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES, NULL);
            }
            gtDmaTransfer(DMA_ENG_2, 0x1f00100, VIRTUAL_TO_PHY(zBuffer) , 640*480*4,
                          DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES | DMA_HOLD_SOURCE_ADDR, NULL);
            if(viewTransformation(&floor[0]) == 1)
            {
                projection();
                while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                flatShade(0);
            }
            for(k = 0 ; k < ballNumObj ; k++)
            {
                if(t[k] < t0[k])
                {
                    if(v[k] == 0xfffffff)
                    {
                        if(viewTransformation(&ball[k]) == 1)
                        {
                            projection();
                            while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                            gouraudShade();
                            //shadow();
                        }
                        continue;
                    }
                    flag = 0;
                    if(t[k] == 0 && v0[k] > 0)
                    {
                        scaleArbitrary(mat, ball[k].pos[0], ball[k].pos[1], ball[k].pos[2], 0, 1.25, 0.9);
                        transformObject(&ball[k],mat);
                    }
                    if((t[k] + 1) < t0[k])
                        (t[k])++;
                    else
                        t[k] = t0[k];
                    v[k] = -10 * t[k] + v0[k];
                    if((v[k] < 0) && (v[k] + ball[k].pos[1]) < 30)
                        move(mat,0 , -ball[k].pos[1]+ 30, 0);
                    else
                        move(mat,0 , v[k], 0);
                    transformObject(&ball[k], mat);
                }
                else
                {
                    if(ball[k].pos[1] > 30)
                    {
                        t0[k] = sqrt(0.2 * (ball[k].pos[1]));
                        v0[k] = 0;
                        if(ball[k].pos[1] > 30)
                            flag = 0;
                    }
                    else
                    {
                        scaleArbitrary(mat, ball[k].pos[0], ball[k].pos[1], ball[k].pos[2], 0, 0.8, 1.1111);
                        transformObject(&ball[k],mat);
                        v0[k] = -v[k] - 10;
                        t0[k] = (0.2 * v0[k]/2) * 0.95;
                        if(t0[k] >= 2)
                            flag = 0;
                        else
                        {
                            v[k] = 0xfffffff;
                            scaleArbitrary(mat, ball[k].pos[0], ball[k].pos[1], ball[k].pos[2], 0, 1.25, 0.9);
                            transformObject(&ball[k],mat);
                        }
                    }
                    t[k] = 0;
                }
                if(viewTransformation(&ball[k]) == 1)
                {
                    projection();
                    while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                    gouraudShade();
                    shadow();
                }
            }
            //lightDisplay();
            REFRESH_SCREEN();
        }
        for(k = 0 ; k < ballNumObj ; k++)
        {
            move(mat, 0, -(ball[k].pos[1]-30), 0);
            transformObject(&ball[k], mat);
        }
        
        /*motorola logo appears */
        for(j = 0 ; j < 120 ; j++)
        {
            move(mat,0,5,0);
            transformObject(&motorola[0], mat);
            move(mat,8,1.5,0);
            rotateArbitrary(mat1,motorola[1].pos[0],motorola[1].pos[1],motorola[1].pos[2],0,0,10,12);
            matrixMul(mat, mat1);
            transformObject(&motorola[1], mat);
    
            while(gtDmaIsChannelActive(DMA_ENG_0));
            for(i = 0 ; i < 480 ; i++)
            {
                while(gtDmaIsChannelActive(0));
                gtDmaTransfer(DMA_ENG_0,bubba.data + i * 1920 , VIRTUAL_TO_PHY(frameBuffer) + (i) * 2048, 1920,         //0x1a00000
                              DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES, NULL);
            }
            gtDmaTransfer(DMA_ENG_2, 0x1f00100, VIRTUAL_TO_PHY(zBuffer) , 640*480*4,
                          DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES | DMA_HOLD_SOURCE_ADDR, NULL);
            if(viewTransformation(&floor[0]) == 1)
            {
                projection();
                while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                flatShade(0);
            }
            for(k = 0 ; k < motorolaNumObj ; k++)
                if(viewTransformation(&motorola[k]) == 1)
                {
                    projection();
                    while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                    gouraudShade();
                    shadow();
                }
            for(k = 0 ; k < ballNumObj ; k++)
                if(viewTransformation(&ball[k]) == 1)
                {
                    projection();
                    while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                    gouraudShade();
                   // shadow();
                }
            //lightDisplay();
            REFRESH_SCREEN();
        }
    
        /* Marvell logo appears */
        for(j = 0 ; j < 180 ; j++)
        {
            while(gtDmaIsChannelActive(DMA_ENG_0));
            for(i = 0 ; i < 480 ; i++)
            {
                while(gtDmaIsChannelActive(0));
                gtDmaTransfer(DMA_ENG_0,bubba.data + i * 1920 , VIRTUAL_TO_PHY(frameBuffer) + (i) * 2048, 1920,         //0x1a00000
                              DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES, NULL);
            }
            gtDmaTransfer(DMA_ENG_2, 0x1f00100, VIRTUAL_TO_PHY(zBuffer) , 640*480*4,
                          DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES | DMA_HOLD_SOURCE_ADDR, NULL);
            if(viewTransformation(&floor[0]) == 1)
            {
                projection();
                while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                flatShade(0);
            }
            for(k = 0 ; k < marvellNumObj ; k++)
            {
                switch(k)
                {
                case 0: 
                    move(mat2,-4, -4, -4);                 
                    rotateArbitrary(mat1, marvell[k].pos[0], marvell[k].pos[1], marvell[k].pos[2], 0, 0, 10, 20);
                    break;
                case 1: 
                    move(mat2,-4, 4, 4); 
                    rotateArbitrary(mat1, marvell[k].pos[0], marvell[k].pos[1], marvell[k].pos[2], 0, 10, 0, 20);
                    break;
                case 2: 
                    move(mat2, 0, 0, -4); 
                    rotateArbitrary(mat1, marvell[k].pos[0], marvell[k].pos[1], marvell[k].pos[2], 10, 0, 0, -20);
                    break;
                case 3: 
                    move(mat2, 0, 4, 4); 
                    rotateArbitrary(mat1, marvell[k].pos[0], marvell[k].pos[1], marvell[k].pos[2], 10, 10, 0, 20);
                    break;
                case 4: 
                    move(mat2, 0, 4, -4); 
                    rotateArbitrary(mat1, marvell[k].pos[0], marvell[k].pos[1], marvell[k].pos[2], 10, 0, 10, -20);
                    break;
                case 5: 
                    move(mat2, 0, -4, 4); 
                    rotateArbitrary(mat1, marvell[k].pos[0], marvell[k].pos[1], marvell[k].pos[2], 0, 10, 10, 20);
                    break;
                case 6: 
                    move(mat2, 0, -4, -4); 
                    rotateArbitrary(mat1, marvell[k].pos[0], marvell[k].pos[1], marvell[k].pos[2], 10, 10, 10, -20);
                    break;
                case 7: 
                    move(mat2,-4, 4, -4); 
                    rotateArbitrary(mat1, marvell[k].pos[0], marvell[k].pos[1], marvell[k].pos[2], 0, 0, 10, 20);
                    break;
                case 8: 
                    move(mat2,-4, -4, 4); 
                    rotateArbitrary(mat1, marvell[k].pos[0], marvell[k].pos[1], marvell[k].pos[2], 0, 10, 0, -20);
                    break;
                case 9: 
                    move(mat2, 4, 0, 0); 
                    rotateArbitrary(mat1, marvell[k].pos[0], marvell[k].pos[1], marvell[k].pos[2], 10, 0, 0, 20);
                    break;
                }
                matrixMul(mat2,mat1);
                transformObject(&marvell[k], mat2);
                if(viewTransformation(&marvell[k]) == 1)
                {
                    projection();
                    while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                    gouraudShade();
                    shadow();
                }
            }
            for(k = 0 ; k < motorolaNumObj ; k++)
                if(viewTransformation(&motorola[k]) == 1)
                {
                    projection();
                    while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                    gouraudShade();
                    shadow();
                }
                for(k = 0 ; k < ballNumObj ; k++)
                    if(viewTransformation(&ball[k]) == 1)
                    {
                        projection();
                        while(gtDmaIsChannelActive(DMA_ENG_1) || gtDmaIsChannelActive(DMA_ENG_2));
                        gouraudShade();
                   //     shadow();
                    }
            //lightDisplay();
            REFRESH_SCREEN();
        }
    
        /* Main loop */
        x[0] = 6;
        z[0] = 3;
        x[1] = -4;
        z[1] = 6;
        x[2] = 5;
        z[2] = -4;
        x[3] = 4;
        z[3] = 6;
        x[4] = -6;
        z[4] = -4;
        alpha[0] =20;
        alpha[1] =20;
        alpha[2] =20;
        alpha[3] =20;
        alpha[4] =20;
        mx=0;
        my=0;
        mz=0;
        vec[0] = motorola[0].pos[0];
        vec[1] = motorola[0].pos[1];
        vec[2] = motorola[0].pos[2];
        for(i = 0 ; i < marvellNumObj ; i++)
        {
            mx += marvell[i].pos[0];
            my += marvell[i].pos[1];
            mz += marvell[i].pos[2];
        }
        mx/= (marvellNumObj);
        my/= (marvellNumObj);
        mz/= (marvellNumObj);
        printf("marv x=%f y=%f z=%f\n",mx,my,mz);
        j = 0;
        while(j < 4000)
        {
            if(j == 0)
                gtAuxilMeasureTime(CNTMR_0,mvTclock);

            j++;
            if(j>100)
                j = 100;
            if(j > 3600)
            {
                transformCamera(-10-((int)(j-3600)*3),0,0,0,0,(int)((j-3600)/10));
            }
            // init frame buffer
            for(i = 0 ; i < 480 ; i++)
            {
                while(gtDmaIsChannelActive(0));
                gtDmaTransfer(DMA_ENG_0,bubba.data + i * 1920 , VIRTUAL_TO_PHY(frameBuffer) + (i) * 2048, 1920,         //0x1a00000
                              DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES, NULL);
            }
            // init Z buffer
            gtDmaTransfer(DMA_ENG_2, 0x1f00100, VIRTUAL_TO_PHY(zBuffer) , 640*480*4,
                          DMA_BLOCK_TRANSFER_MODE | DMA_DTL_32BYTES | DMA_HOLD_SOURCE_ADDR, NULL);

            // calculate balls movement
            move(mat, x[0], 0, z[0]);
            if((10*z[0] - (-10*x[0]/z[0])*x[0]) >= 0)
                i = -10;
            else
                i = 10;
            rotateArbitrary(mat1, ball[0].pos[0], ball[0].pos[1], ball[0].pos[2],i,0,-i*x[0]/z[0],alpha[0]);
            matrixMul(mat, mat1);
            transformObject(&ball[0], mat);
            move(mat, x[1], 0, z[1]);
            if((10*z[1] - (-10*x[1]/z[1])*x[1]) > 0)
                i = -10;
            else
                i = 10;
            rotateArbitrary(mat1, ball[1].pos[0], ball[1].pos[1], ball[1].pos[2],i,0,-i*x[1]/z[1],alpha[1]);
            matrixMul(mat, mat1);
            transformObject(&ball[1], mat);
            move(mat, x[2], 0, z[2]);
            if((10*z[2] - (-10*x[2]/z[2])*x[2]) > 0)
                i = -10;
            else
                i = 10;
            rotateArbitrary(mat1, ball[2].pos[0], ball[2].pos[1], ball[2].pos[2], i,0,-i*x[2]/z[2],alpha[2]);
            matrixMul(mat, mat1);
            transformObject(&ball[2], mat);
            move(mat, x[3], 0, z[3]);
            if((10*z[3] - (-10*x[3]/z[3])*x[3]) > 0)
                i = -10;
            else
                i = 10;
            rotateArbitrary(mat1, ball[3].pos[0], ball[3].pos[1], ball[3].pos[2], i,0,-i*x[3]/z[3],alpha[3]);
            matrixMul(mat, mat1);
            transformObject(&ball[3], mat);
            move(mat, x[4], 0, z[4]);
            if((10*z[4] - (-10*x[4]/z[4])*x[4]) > 0)
                i = -10;
            else
                i = 10;
            rotateArbitrary(mat1, ball[4].pos[0], ball[4].pos[1], ball[4].pos[2], i,0,-i*x[4]/z[4],alpha[4]);
            matrixMul(mat, mat1);
            transformObject(&ball[4], mat);
        
            // put floor in frame buffer
            if(viewTransformation(&floor[0]) == 1)
            {
                projection();
                while(gtDmaIsChannelActive(DMA_ENG_2));
                flatShade(0);
            }
            // put marvell in frame buffer
            for(k = 0 ; k < marvellNumObj ; k++)
            {
                rotateArbitrary(mat, mx, my, mz, 0, 10, 0, 3);
                marvellAngle = (marvellAngle + 3)%360;
                transformObject(&marvell[k], mat);
                if(viewTransformation(&marvell[k]) == 1)
                {
                    projection();
                    while(gtDmaIsChannelActive(DMA_ENG_2));
                    gouraudShade();
                    shadow();
                }
            }
            // calculate and display motorola
            rotateArbitrary(mat1,motorola[1].pos[0] ,motorola[1].pos[1] ,motorola[1].pos[2], 0, 5, 0, -15);
            motRingAngle = (motRingAngle + 15)%360;
            transformObject(&motorola[1], mat1);
            rotateArbitrary(mat1,mx ,my ,mz, 4, 0, 8, 2);
            vectorMatrixMul(vec,mat1);
            move(mat,vec[0]-motorola[0].pos[0],vec[1]-motorola[0].pos[1],vec[2]-motorola[0].pos[2]);
            transformObject(&motorola[1], mat);
            transformObject(&motorola[0], mat);
            vec[0] = motorola[0].pos[0];
            vec[1] = motorola[0].pos[1];
            vec[2] = motorola[0].pos[2];
            for(k = 0 ; k < motorolaNumObj ; k++)
                if(viewTransformation(&motorola[k]) == 1)
                {
                    projection();
                    while(gtDmaIsChannelActive(DMA_ENG_2));
                    gouraudShade();
                    shadow();
                }
            // More ball calculations and display
            bflag[0] = bflag[1] = bflag[2] = bflag[3] = bflag[4] =0;
            for(k = 0 ; k < ballNumObj ; k++)
            {
                flag = 0;
                if((ball[k].pos[0] < -240 || ball[k].pos[0] > 850) && bflag[k] == 0)
                {
                    if((ball[k].pos[0] < -240 && x[k] < 0) || (ball[k].pos[0] > 850 && x[k] > 0))
                        x[k] = -x[k];
                    flag = 1;
                }
                if((ball[k].pos[2] < -450 || ball[k].pos[2] > 650) && bflag[k] == 0)
                {
                    if((ball[k].pos[2] < -450 && z[k] < 0) || (ball[k].pos[2] > 650 && z[k] > 0))
                        z[k] = -z[k];
                    flag = 1;
                }
                if(flag == 1)
                    bflag[k] = 1;
                for(i = 0 ; i < ballNumObj ; i++)
                {
                    if(i == k)
                        continue;
                    if(sqrt(pow(ball[k].pos[0]-ball[i].pos[0], 2)+pow(ball[k].pos[2]-ball[i].pos[2], 2)) <=60
                       && bflag[k] == 0)
                    {
                        bflag[i] = 1;
                        if(ball[k].pos[0]-ball[i].pos[0] >= 30 || ball[k].pos[0]-ball[i].pos[0] <= -30)
                        {
                            x[k]= -x[k];
                            x[i] = -x[i];
                        }
                        if(ball[k].pos[2]-ball[i].pos[2] >= 30 || ball[k].pos[2]-ball[i].pos[2] <= -30)
                        {
                            z[k] = -z[k];
                            z[i] = -z[i];
                        }
                    }
                }
                if(viewTransformation(&ball[k]) == 1)
                {
                    projection();
                    while(gtDmaIsChannelActive(DMA_ENG_2));
                    gouraudShade();
                }
            }
            for(k = 0 ; k < atlantisNumObj ; k++)
            {
                rotateArbitrary(mat, 800, 420, 200, 0, 10, 0, 1);
                if(atlantis[0].pos[1] >= 420)
                {
                    move(mat1, 0, -10, 0);
                    matrixMul(mat, mat1);
                }
                transformObject(&atlantis[k], mat);
                if(viewTransformation(&atlantis[k]) == 1)
                {
                    projection();
                    while(gtDmaIsChannelActive(DMA_ENG_2));
                    gouraudShade();
                }
            }
            REFRESH_SCREEN();
            if(j == 50)
            {
                time = gtAuxilMeasureTime(CNTMR_0,mvTclock);
                printf("Iteration time = %f\n",time);
            }
            
        }
  
        /* Return all objects to initial position*/
        
        for(k = 0 ; k < RUReadyNumObj; k++)        
        {
            move(mat, -(RUReady[k].pos[0]+5200), 0, 0);
            transformObject(&RUReady[k],mat);
        }
        for(k = 0 ; k < numbersNumObj; k++)        
        {
            move(mat, -(cNumber[k].pos[0]+5200), 0, 0);
            transformObject(&cNumber[k],mat);
        }
        camera.pos[0] = -5000;
        camera.pos[1] = 700;
        camera.pos[2] = 100;
        camera.pitch = -5;
        camera.tilt = 0;
        for(k = 0 ; k < ballNumObj; k++)        
        {
            move(mat, 0, 800 + k*50, 0);
            transformObject(&ball[k],mat);
        }
    
        for(k = 0 ; k < marvellNumObj; k++)        
        {
            rotateArbitrary(mat,mx, my, mz, 0, 10, 0, -marvellAngle);
            transformObject(&marvell[k],mat);
        }
        printf("marvang=%d\n",marvellAngle);
        marvellAngle=0;
        /* Marvell starting positions */
        move(mat, 720, 720 , 720);
        transformObject(&marvell[0],mat);
        move(mat, 720, -720, -720);
        transformObject(&marvell[1],mat);
        move(mat, 0, 0, 720);
        transformObject(&marvell[2],mat);
        move(mat, 0, -720, -720);
        transformObject(&marvell[3],mat);
        move(mat, 0, -720, 720);
        transformObject(&marvell[4],mat);
        move(mat, 0, 720, -720);
        transformObject(&marvell[5],mat);
        move(mat, 0, 720, 720);
        transformObject(&marvell[6],mat);
        move(mat, 720, -720, 720);
        transformObject(&marvell[7],mat);
        move(mat, 720, 720, -720);
        transformObject(&marvell[8],mat);
        move(mat, -720, 0, 0);
        transformObject(&marvell[9],mat);
    
        /*motorola*/
        move(mat, -(motorola[0].pos[0]-87.5), -(motorola[0].pos[1]+175.384), -(motorola[0].pos[2]-100));
        transformObject(&motorola[0], mat);
        rotateArbitrary(mat1,motorola[1].pos[0], motorola[1].pos[1], motorola[1].pos[2], 0, 10, 0, motRingAngle);
        printf("motringang=%d\n",motRingAngle);
        motRingAngle=0;
        move(mat, -(motorola[1].pos[0]+872.5), -(motorola[1].pos[1]-250), -(motorola[1].pos[2]-100));
        matrixMul(mat, mat1);
        transformObject(&motorola[1],mat);
        printf("okok\n");
        printf("mot0 x=%f y=%f z=%f\n",motorola[0].pos[0],motorola[0].pos[1],motorola[0].pos[2]);
        printf("mot1 x=%f y=%f z=%f\n",motorola[1].pos[0],motorola[1].pos[1],motorola[1].pos[2]);

        for(k = 0 ; k < atlantisNumObj ; k++)
        {
            move(mat, 0, 580, 0);
            transformObject(&atlantis[k], mat);
        }
    }
}
