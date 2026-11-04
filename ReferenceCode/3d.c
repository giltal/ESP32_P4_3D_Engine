#include "3d.h"
#include "..\drivers\ev64360.h"
#include "..\drivers\gtFS.h"
#include "..\drivers\gtSysGal.h"
#include "..\drivers\gtDma.h"
#include "..\drivers\mvAltiVec.h"
#include <math.h>


float cosAr[360], sinAr[360];
int objIdList[200];
unsigned char * frameBuffer;
int * zBuffer;
CAMERA camera;
LIGHT light;
FINALOBJECT final;

extern MV_MATRIX_4X4 * t1,*t2,*t3,*t4,*t5,*t6;

void initColor()
{
    int b ;
 /*   for(r = 0 ; r < 6 ; r++)
        for(g= 0 ; g < 6 ; g++)
            for(b= 0 ; b < 6 ; b++)
                cirrusOverridePaletteReg((r * 6 * 6) + (g * 6) + b, r * 10, g * 10 , b * 10 );*/
    /* Gray scale */
      for(b= 0 ; b < 32 ; b++)
      {
        cirrusOverridePaletteReg(b, b * 2, b * 2 , b * 2);
      }
    /* Red scale */
      for(b= 0 ; b < 32 ; b++)
      {
        cirrusOverridePaletteReg(32+b, 63, b*2 , b*2);
      }
    /* Green scale */
      for(b= 0 ; b < 32 ; b++)
      {
        cirrusOverridePaletteReg(64 + b, b*2, 63 , b*2);
      }
    /* Blue scale */
      for(b= 0 ; b < 32 ; b++)
      {
        cirrusOverridePaletteReg(96 + b, b*2, b*2, 63);
      }
    /* Yellow scale */
      for(b= 0 ; b < 32 ; b++)
      {
        cirrusOverridePaletteReg(128 + b, 63, 63 , b*2);
      }
    /* Purple scale */
      for(b= 0 ; b < 32 ; b++)
      {
        cirrusOverridePaletteReg(160 + b, 63, b*2 , 63);
      }
    /* Torkois scale */
      for(b= 0 ; b < 32 ; b++)
      {
        cirrusOverridePaletteReg(192 + b, b*2, 63 , 63);
      }
}

void init3d()
{
    int i;
    float x;
    //initColor();
    printf("1\n");
    frameBuffer = ((unsigned char *) malloc(2048 * VERTICAL));
    zBuffer = ((int *) malloc(HORIZONTAL * VERTICAL * 4));
    printf("2\n");
    camera.pos[0]= 0;
    camera.pos[1]= 0;
    camera.pos[2]=0;
    camera.heading = 0;
    camera.pitch = 0;
    camera.tilt = 0;
    for(i = 0 ; i < 360 ; i++)
    {
        if(i == 90)
        {
            cosAr[i] = 0;
            sinAr[i] = 1;
            continue;
        }
        else if(i == 180)
        {
            cosAr[i] = -1;
            sinAr[i] = 0;
            continue;
        }
        else if(i == 270)
        {
            cosAr[i] = 0;
            sinAr[i] = -1;
            continue;
        }
        else
            x =  i / (DEGREE_CONV);
        cosAr[i] = cosf(x);
        sinAr[i] = sinf(x);
    }
    printf("3\n");
}
/*
void matrixMul(MV_MATRIX_4X4 * m1, MV_MATRIX_4X4 * m2)
{
    int i, j;
    MV_MATRIX_4X4 mat;

    for(i = 0 ; i < 4 ; i++)
        for(j = 0 ; j < 4 ; j++)
        {
            mat.m[i][j] = (m1->m[i][0] * m2->m[0][j]) + (m1->m[i][1] * m2->m[1][j]) +
                       (m1->m[i][2] * m2->m[2][j]) + (m1->m[i][3] * m2->m[3][j]); 
        }
        m1->m[0][0] = mat.m[0][0];
        m1->m[0][1] = mat.m[0][1];
        m1->m[0][2] = mat.m[0][2];
        m1->m[0][3] = mat.m[0][3];
        m1->m[1][0] = mat.m[1][0];
        m1->m[1][1] = mat.m[1][1];
        m1->m[1][2] = mat.m[1][2];
        m1->m[1][3] = mat.m[1][3];
        m1->m[2][0] = mat.m[2][0];
        m1->m[2][1] = mat.m[2][1];
        m1->m[2][2] = mat.m[2][2];
        m1->m[2][3] = mat.m[2][3];
        m1->m[3][0] = mat.m[3][0];
        m1->m[3][1] = mat.m[3][1];
        m1->m[3][2] = mat.m[3][2];
        m1->m[3][3] = mat.m[3][3];   
}   
*/
/* m1 = m1*m2 */

void matrixMul(MV_MATRIX_4X4 * m1, MV_MATRIX_4X4 * m2)
{
    // Load the Vectors with Matrix data
    MV_AV_LOAD_VEC(1,m1->m[0]);
    MV_AV_LOAD_VEC(2,m1->m[1]);
    MV_AV_LOAD_VEC(3,m1->m[2]);
    MV_AV_LOAD_VEC(4,m1->m[3]);

    MV_AV_LOAD_VEC(5,m2->m[0]);
    MV_AV_LOAD_VEC(6,m2->m[1]);
    MV_AV_LOAD_VEC(7,m2->m[2]);
    MV_AV_LOAD_VEC(8,m2->m[3]);

    MV_AV_SPLAT_WORD(11,1,0);
    MV_AV_FP_MUL_ADD(9 ,11,5,0);
    MV_AV_SPLAT_WORD(12,1,1);
    MV_AV_FP_MUL_ADD(10,12,6,9);
    MV_AV_SPLAT_WORD(13,1,2);
    MV_AV_FP_MUL_ADD(9 ,13,7,10);
    MV_AV_SPLAT_WORD(14,1,3);
    MV_AV_FP_MUL_ADD(10,14,8,9);

    MV_AV_STORE_VEC(10,m1->m[0]);
    
    MV_AV_SPLAT_WORD(15,2,0);
    MV_AV_FP_MUL_ADD(9,15,5,0);
    MV_AV_SPLAT_WORD(16,2,1);
    MV_AV_FP_MUL_ADD(10,16,6,9);
    MV_AV_SPLAT_WORD(17,2,2);
    MV_AV_FP_MUL_ADD(9,17,7,10);
    MV_AV_SPLAT_WORD(18,2,3);
    MV_AV_FP_MUL_ADD(10,18,8,9);

    MV_AV_STORE_VEC(10,m1->m[1]);
    
    MV_AV_SPLAT_WORD(19,3,0);
    MV_AV_FP_MUL_ADD(9,19,5,0);
    MV_AV_SPLAT_WORD(20,3,1);
    MV_AV_FP_MUL_ADD(10,20,6,9);
    MV_AV_SPLAT_WORD(21,3,2);
    MV_AV_FP_MUL_ADD(9,21,7,10);
    MV_AV_SPLAT_WORD(22,3,3);
    MV_AV_FP_MUL_ADD(10,22,8,9);

    MV_AV_STORE_VEC(10,m1->m[2]);
    
    MV_AV_SPLAT_WORD(23,4,0);
    MV_AV_FP_MUL_ADD(9,23,5,0);
    MV_AV_SPLAT_WORD(24,4,1);
    MV_AV_FP_MUL_ADD(10,24,6,9);
    MV_AV_SPLAT_WORD(25,4,2);
    MV_AV_FP_MUL_ADD(9,25,7,10);
    MV_AV_SPLAT_WORD(26,4,3);
    MV_AV_FP_MUL_ADD(10,26,8,9);

    MV_AV_STORE_VEC(10,m1->m[3]);
}

void vectorMatrixMul(float vec[], MV_MATRIX_4X4 * mat)
{
    int i; 
    float tmpVec[3];
    for(i = 0 ; i < 3 ; i++)
        tmpVec[i] = (vec[0] * mat->m[i][0]) + (vec[1] * mat->m[i][1]) +
                 (vec[2] * mat->m[i][2]) + (mat->m[i][3]);
    vec[0] = tmpVec[0];
    vec[1] = tmpVec[1];
    vec[2] = tmpVec[2];
} 

void  move(MV_MATRIX_4X4 * mat, float dx, float dy, float dz)
{
    mat->m[0][0] = 1;
    mat->m[0][1] = 0;
    mat->m[0][2] = 0;
    mat->m[0][3] = dx;
    mat->m[1][0] = 0;
    mat->m[1][1] = 1;
    mat->m[1][2] = 0;
    mat->m[1][3] = dy;
    mat->m[2][0] = 0;
    mat->m[2][1] = 0;
    mat->m[2][2] = 1;
    mat->m[2][3] = dz;
    mat->m[3][0] = 0;
    mat->m[3][1] = 0;
    mat->m[3][2] = 0;
    mat->m[3][3] = 1;
}

void scale(MV_MATRIX_4X4 * mat, float sx, float sy, float sz)
{
    if(sx == 0)
        sx = 1;
    if(sy == 0)
        sy = 1;
    if(sz == 0)
        sz = 1;
    /* object coordinates transformation */
    mat->m[0][0] = sx;
    mat->m[0][1] = 0;
    mat->m[0][2] = 0;
    mat->m[0][3] = 0;
    mat->m[1][0] = 0;
    mat->m[1][1] = sy;
    mat->m[1][2] = 0;
    mat->m[1][3] = 0;
    mat->m[2][0] = 0;
    mat->m[2][1] = 0;
    mat->m[2][2] = sz;
    mat->m[2][3] = 0;
    mat->m[3][0] = 0;
    mat->m[3][1] = 0;
    mat->m[3][2] = 0;
    mat->m[3][3] = 1;
}

void rotate(MV_MATRIX_4X4 * mat, int x, int y, int z)
{
    if(x != 0)
    {
        mat->m[0][0] = 1;
        mat->m[0][1] = 0;
        mat->m[0][2] = 0;
        mat->m[0][3] = 0;
        mat->m[1][0] = 0;
        mat->m[1][1] = gcos(x);
        mat->m[1][2] = gsin(x);
        mat->m[1][3] = 0;
        mat->m[2][0] = 0;
        mat->m[2][1] = -gsin(x);
        mat->m[2][2] = gcos(x);
        mat->m[2][3] = 0;
        mat->m[3][0] = 0;
        mat->m[3][1] = 0;
        mat->m[3][2] = 0;
        mat->m[3][3] = 1;
    }
    if(y != 0)
    {
        mat->m[0][0] = gcos(y);
        mat->m[0][1] = 0;
        mat->m[0][2] = -gsin(y);
        mat->m[0][3] = 0;
        mat->m[1][0] = 0;
        mat->m[1][1] = 1;
        mat->m[1][2] = 0;
        mat->m[1][3] = 0;
        mat->m[2][0] = gsin(y);
        mat->m[2][1] = 0;
        mat->m[2][2] = gcos(y);
        mat->m[2][3] = 0;
        mat->m[3][0] = 0;
        mat->m[3][1] = 0;
        mat->m[3][2] = 0;
        mat->m[3][3] = 1;
    }
    if(z != 0)
    {
        mat->m[0][0] = gcos(z);
        mat->m[0][1] = -gsin(z);
        mat->m[0][2] = 0;
        mat->m[0][3] = 0;
        mat->m[1][0] = gsin(z);
        mat->m[1][1] = gcos(z);
        mat->m[1][2] = 0;
        mat->m[1][3] = 0;
        mat->m[2][0] = 0;
        mat->m[2][1] = 0;
        mat->m[2][2] = 1;
        mat->m[2][3] = 0;
        mat->m[3][0] = 0;
        mat->m[3][1] = 0;
        mat->m[3][2] = 0;
        mat->m[3][3] = 1;
    }
    if(x==0 && y==0 && z==0)
    {
        mat->m[0][0] = 1;
        mat->m[0][1] = 0;
        mat->m[0][2] = 0;
        mat->m[0][3] = 0;
        mat->m[1][0] = 0;
        mat->m[1][1] = 1;
        mat->m[1][2] = 0;
        mat->m[1][3] = 0;
        mat->m[2][0] = 0;
        mat->m[2][1] = 0;
        mat->m[2][2] = 1;
        mat->m[2][3] = 0;
        mat->m[3][0] = 0;
        mat->m[3][1] = 0;
        mat->m[3][2] = 0;
        mat->m[3][3] = 1;
    }
}

void scaleArbitrary(MV_MATRIX_4X4 * mat, float basex, float basey, float basez, 
                     float xAxis, float yAxis, float zAxis)
{
    scale(mat, 0, 0, 0);
    move(t1, -basex, -basey, -basez);
    scale(t2, xAxis, yAxis, zAxis);
    move(t3, basex, basey, basez);
    matrixMul(mat, t3);
    matrixMul(mat, t2);
    matrixMul(mat, t1);
}

void rotateArbitrary(MV_MATRIX_4X4 * mat, float basex, float basey, float basez, 
                     int xAxis, int yAxis, int zAxis, int alpha)
{
    float dxz;
    int phi, teta;

    scale(mat, 0, 0, 0);
    move(t1, -basex, -basey, -basez);
    dxz = xAxis * xAxis + zAxis * zAxis;
    if(dxz < 10)
    {
        if(yAxis < 0)
            alpha = -alpha;
        rotate(t2 , 0, alpha, 0);
        move(t3, basex, basey, basez);
        matrixMul(t3, t2);
        matrixMul(t3, t1);
        matrixMul(mat, t3);
        return;
    }
    dxz = sqrt(dxz);
    if(xAxis == 0 && zAxis > 0)
        phi = 90;
    else if(xAxis == 0 && zAxis < 0)
        phi = -90;
    else
        phi= (int)(DEGREE_CONV * atan2(zAxis, xAxis));
    teta = (int)(DEGREE_CONV * atan2(yAxis, dxz));
    rotate(t2, 0, (-phi), 0);
    rotate(t3, 0, 0, -teta);
    rotate(t4, alpha, 0, 0);
    rotate(t5, 0, 0, teta);
    rotate(t6, 0, phi, 0);
    move(mat, basex, basey, basez);
    matrixMul(mat, t6);
    matrixMul(mat, t5);
    matrixMul(mat, t4);
    matrixMul(mat, t3);
    matrixMul(mat, t2);
    matrixMul(mat, t1);
}

void transformObject(OBJECT * pObject, MV_MATRIX_4X4 * mat)
{
    int i, cameraDist;
    float tmp[3];
    VERTEX * vertex;

    pObject->max = 0;
    pObject->min = 0xfffffff;
    vertex = pObject->pVertex;
    for(i = 0 ; i < pObject->numVeritces ; i++)
    {
        vectorMatrixMul(vertex[i].pos, mat);
        cameraDist = pow((vertex[i].pos[0] - camera.pos[0]),2) +
            pow((vertex[i].pos[1] - camera.pos[1]),2) +
            pow((vertex[i].pos[2] - camera.pos[2]),2);
        if(cameraDist < pObject->min)
            pObject->min = cameraDist; 
        if(cameraDist > pObject->max)
            pObject->max = cameraDist; 
    } 
    /* object coordinates transformation */
    vectorMatrixMul(pObject->pos, mat);
    /*normals transform */
    for(i = 0 ; i < pObject->numPolygons ; i++)
    {
        tmp[0] = mat->m[0][0] * pObject->pPolygon[i].n[0] + mat->m[0][1] * pObject->pPolygon[i].n[1] +
            mat->m[0][2] * pObject->pPolygon[i].n[2];
        tmp[1] = mat->m[1][0] * pObject->pPolygon[i].n[0] + mat->m[1][1] * pObject->pPolygon[i].n[1] +
            mat->m[1][2] * pObject->pPolygon[i].n[2];
        tmp[2] = mat->m[2][0] * pObject->pPolygon[i].n[0] + mat->m[2][1] * pObject->pPolygon[i].n[1] +
            mat->m[2][2] * pObject->pPolygon[i].n[2];
        pObject->pPolygon[i].n[0] = tmp[0];
        pObject->pPolygon[i].n[1] = tmp[1];
        pObject->pPolygon[i].n[2] = tmp[2];
    }
    for(i = 0 ; i < pObject->numVeritces ; i++)
    {
        if(pObject->pVertex[i].n[0] == 0xfffffff && pObject->pVertex[i].n[1] == 0xfffffff &&
           pObject->pVertex[i].n[2] == 0xfffffff)
            continue;
        tmp[0] = mat->m[0][0] * pObject->pVertex[i].n[0] + mat->m[0][1] * pObject->pVertex[i].n[1] +
            mat->m[0][2] * pObject->pVertex[i].n[2];
        tmp[1] = mat->m[1][0] * pObject->pVertex[i].n[0] + mat->m[1][1] * pObject->pVertex[i].n[1] +
            mat->m[1][2] * pObject->pVertex[i].n[2];
        tmp[2] = mat->m[2][0] * pObject->pVertex[i].n[0] + mat->m[2][1] * pObject->pVertex[i].n[1] +
            mat->m[2][2] * pObject->pVertex[i].n[2];
        pObject->pVertex[i].n[0] = tmp[0];
        pObject->pVertex[i].n[1] = tmp[1];
        pObject->pVertex[i].n[2] = tmp[2];
    }
}

void transformCamera(float dx, float dy, float dz, int dHeading, int dPitch, int dTilt)
{

    camera.pos[0] += dx;
    camera.pos[1] += dy;
    camera.pos[2] += dz;
    camera.heading = (camera.heading + dHeading) % 360;
    camera.pitch = (camera.pitch + dPitch) % 360;
    camera.tilt = (camera.tilt + dTilt) % 360;
    camera.direction[0] = gcos(camera.pitch) * gcos(camera.heading);
    camera.direction[1] = gsin(camera.pitch);
    camera.direction[2] = gcos(camera.pitch) * gsin(camera.heading);
}

void transformLight(float dx, float dy, float dz, int dHeading, int dPitch, int dTilt, int R, int G, int B)
{
    if(!(R == 0 && G==0 && B==0))
    {
        light.R = R;
        light.G = G;
        light.B = B;
    }
    light.pos[0] += dx;
    light.pos[1] += dy;
    light.pos[2] += dz;
    light.heading = (light.heading + dHeading) % 360;
    light.pitch = (light.pitch + dPitch) % 360;
    light.tilt = (light.tilt + dTilt) % 360;
    light.direction[0] = gcos(light.pitch) * gcos(light.heading);
    light.direction[1] = gsin(light.pitch);
    light.direction[2] = gcos(light.pitch) * gsin(light.heading);
}

int viewTransformation(OBJECT * pObject)
{
    VERTEX * vertex;
    int i, j, seenFlag=1;

    final.pCurrentObj = pObject;
    move(t1, -camera.pos[0], -camera.pos[1], -camera.pos[2]);
    rotate(t2, 0, -camera.heading, 0);
    rotate(t3, 0, 0, -camera.pitch);
    rotate(t4, -camera.tilt, 0, 0);
    matrixMul(t4, t3);
    matrixMul(t4, t2);
    matrixMul(t4, t1);
    vertex = pObject->pVertex;
    for(i = 0 ; i < pObject->numVeritces ; i++)
    {
        for(j = 0 ; j < 3 ; j++)
        {
            final.vertexList[i].pos[j] = (vertex->pos[0] * t4->m[j][0]) + (vertex->pos[1] * t4->m[j][1]) +
                     (vertex->pos[2] * t4->m[j][2]) + (t4->m[j][3]);
        }
        if(final.vertexList[i].pos[0] < 0 && (pObject->status & BIT2) != BIT2)
            seenFlag = 0;
        else if(final.vertexList[i].pos[0] < 0 && (pObject->status & BIT2) == BIT2)
            final.vertexList[i].pos[0] = 1;
        vertex++;
    }

    /* transformation of polygon normal.x for culling in projection if normal.x > 0 */
    /*also if after view transform x<0 this object is not projected */
    for(i = 0 ; i < pObject->numPolygons ; i++)
    {
        final.viewingXNormal[i] = (pObject->pPolygon[i].n[0] * t4->m[0][0]) + (pObject->pPolygon[i].n[1] * t4->m[0][1]) +
                     (pObject->pPolygon[i].n[2] * t4->m[0][2]);
    }

    j=0;
    for(i = 0 ; i < final.pCurrentObj->numPolygons ; i++)
    {
        while(i < final.pCurrentObj->numPolygons && ((final.viewingXNormal[i] > .3 && (final.pCurrentObj->status & BIT3) ==0) || 
                                                     (final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[0] < 0 &&
                                                 final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[0] < 0 &&
                                                 final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[0] < 0))) 
        {
            i++;
        }
        if(i == final.pCurrentObj->numPolygons)
        {
            break;
        }
        final.polyIdList[j] = i;
        j++;
    }
    final.seenNumPoly = j;
    return seenFlag;
}

void projection()
{
    int i;
    float sx, sy;

    sx = HORIZONTAL / 42.44 *35 ;  /* sx=Xmax/2 * Fr/21.22*Ax*/
    sy = VERTICAL / 42.44 * 35;
    for(i = 0 ; i < 1000 ; i++)
        final.projX[i] = final.projY[i] = 0;
    for(i = 0 ; i < final.seenNumPoly ; i++)
    {
        if(final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] == 0 &&
           final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] == 0)
        {
            final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] = HORIZONTAL / 2 + 
                (final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[2]) /
                (final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0])* sx;
            final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] = VERTICAL / 2 - 
                (final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[1]) /
                (final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0])* sy;
        }
        if(final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] == 0 &&
           final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] == 0)
        {
            final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] = HORIZONTAL / 2 + 
                (final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[2]) /
                (final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0])* sx;
            final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] = VERTICAL / 2 - 
                (final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[1]) /
                (final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0])* sy;
        }
        if(final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]] == 0 &&
           final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]] == 0)
        {
            final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]] = HORIZONTAL / 2 + 
                (final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[2]) /
                (final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0])* sx;
            final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]] = VERTICAL / 2 - 
                (final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[1]) /
                (final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0])* sy;
        }
    }
                      
}

void sortObjects(OBJECT * obj,int numObjects)
{
    int i, flag;

    objIdList[0] = 0;   // ground plane
    for(i = 1 ; i < numObjects ; i++)
        objIdList[i] = i;
    while(1)
    {
        flag = 0xfffffff;
        for(i = 0 ; i < numObjects-1 ; i++)
        {
            if( obj[objIdList[i]].max > obj[objIdList[i+1]].max)
            {
                flag = objIdList[i+1];
                objIdList[i+1] = objIdList[i];
                objIdList[i] = flag;
            }
        }
        if(flag == 0xfffffff)
            break;
    }
}

void sortPolygon()
{
    int i, j = 0, flag;
    for(i = 0 ; i < final.pCurrentObj->numPolygons ; i++)
    {
        while(i < final.pCurrentObj->numPolygons && ((final.viewingXNormal[i] > 0.5) || (final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[0] < 0 &&
                                                 final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[0] < 0 &&
                                                 final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[0] < 0))) 
        {
            i++;
        }
        if(i == final.pCurrentObj->numPolygons)
        {
            break;
        }
        final.polyIdList[j] = i;
        if(final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[0]].pos[0] >=
           final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[1]].pos[0] &&
           final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[0]].pos[0] >=
           final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[2]].pos[0])
        final.minimalXDistance[i] = pow(
            final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[0]].pos[0],2);
        else if(final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[1]].pos[0] >=
           final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[0]].pos[0] &&
           final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[1]].pos[0] >=
           final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[2]].pos[0])
            final.minimalXDistance[i] = pow( 
                final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[1]].pos[0],2);
        else
            final.minimalXDistance[i] = pow(
                final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[2]].pos[0],2);   
        if(final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[0]].pos[0] <
           final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[1]].pos[0] &&
           final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[0]].pos[0] <
           final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[2]].pos[0])
        final.minimalXDistance[i] += pow(
            final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[0]].pos[0],2);
        else if(final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[1]].pos[0] <
           final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[0]].pos[0] &&
           final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[1]].pos[0] <
           final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[2]].pos[0])
            final.minimalXDistance[i] += pow( 
                final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[1]].pos[0],2);
        else
            final.minimalXDistance[i] += pow(
                final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[2]].pos[0],2);   
        final.minimalXDistance[i] += 
            pow((final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[0]].pos[0] +
            final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[1]].pos[0] +
            final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[2]].pos[0]) / 3,2)+
            pow((final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[0]].pos[1] +
            final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[1]].pos[1] +
            final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[2]].pos[1]) / 3,2)+
            pow((final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[0]].pos[2] +
            final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[1]].pos[2] +
            final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[j]].vertexID[2]].pos[2]) / 3,2);
        j++;
    }
    final.seenNumPoly = j;
    if(j == 0)
        return;
    for(i=0; i<j;i++)
    while(1)                     
    {
        flag = 0xfffffff;
        for(i = 0 ; i < j -1 ; i++)
        {
            if(final.minimalXDistance[final.polyIdList[i]] < final.minimalXDistance[final.polyIdList[i+1]])
            {
                flag = final.polyIdList[i+1];
                final.polyIdList[i+1] = final.polyIdList[i];
                final.polyIdList[i] = flag;
            }
        }
        if(flag == 0xfffffff)
            break;
    }  
}

void lineDifGen(int x1, int y1, int x2, int y2, int R, int G, int B)
{
    int dx, dy, x, y, yinc, xinc, tmp;
    float delt;

    dx = x2 - x1;
    dy = y2 - y1;
    if(abs(dx) > abs(dy))
    {
        if(dx < 0)
        {
            tmp = x2;
            x2 = x1;
            x1 = tmp;
            tmp = y2;
            y2 = y1;
            y1 = tmp;
        }
        if(y2 > y1)
            yinc = 1;
        else
            yinc = -1;
        dx = x2 - x1;
        dy = y2 - y1;
        delt = -dx / 2;
        x = x1;
        y = y1;
        if(x >= 0 && x <= HORIZONTAL && y >= 0 && y <= VERTICAL)
            GT_WRITE_CHAR((frameBuffer + x + (y * HORIZONTAL)), color(R,G,B));
        while(x < x2)
        {
            delt = delt + dy;
            if(delt >= 0)
            {
                y += yinc;
                delt = delt - dx;
            }
            x = x + 1;
            if(x >= 0 && x <= HORIZONTAL && y >= 0 && y <= VERTICAL)
                GT_WRITE_CHAR((frameBuffer + x + (y * HORIZONTAL)), color(R,G,B));
        }
    }
    else
    {
        if(dy < 0)
        {
            tmp = x2;
            x2 = x1;
            x1 = tmp;
            tmp = y2;
            y2 = y1;
            y1 = tmp;
        }
        if(x2 > x1)
            xinc = 1;
        else
            xinc = -1;
        dx = x2 - x1;
        dy = y2 - y1;
        delt = -dy / 2;
        x = x1;
        y = y1;
        if(x >= 0 && x <= HORIZONTAL && y >= 0 && y <= VERTICAL)
            GT_WRITE_CHAR((frameBuffer + x + (y * HORIZONTAL)), color(R,G,B));
        while(y < y2)
        {
            delt = delt + dx;
            if(delt >= 0)
            {
                x += xinc;
                delt = delt - dy;
            }
            y = y + 1;
            if(x >= 0 && x <= HORIZONTAL && y >= 0 && y <= VERTICAL)
                GT_WRITE_CHAR((frameBuffer + x + (y * HORIZONTAL)), color(R,G,B));
        }
    } 
}

void wireframe()
{
    int i, j, x1, x2, y1, y2;
    for(i = 0 ; i < final.seenNumPoly ; i++)
    {
        x1 = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
        y1 = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
        for(j = 0 ; j < final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].numAdjVert ; j++)
        {
            x2 = final.projX[final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pAdjVert[j]];
            y2 = final.projY[final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pAdjVert[j]];
            lineDifGen(x1, y1, x2, y2, final.pCurrentObj->pPolygon[final.polyIdList[i]].R,
                       final.pCurrentObj->pPolygon[final.polyIdList[i]].G,
                       final.pCurrentObj->pPolygon[final.polyIdList[i]].B);
        }
        x1 = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
        y1 = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
        for(j = 0 ; j < final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].numAdjVert ; j++)
        {
            x2 = final.projX[final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pAdjVert[j]];
            y2 = final.projY[final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pAdjVert[j]];
            lineDifGen(x1, y1, x2, y2, final.pCurrentObj->pPolygon[final.polyIdList[i]].R,
                       final.pCurrentObj->pPolygon[final.polyIdList[i]].G,
                       final.pCurrentObj->pPolygon[final.polyIdList[i]].B);
        }
        x1 = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
        y1 = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
        for(j = 0 ; j < final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].numAdjVert ; j++)
        {
            x2 = final.projX[final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pAdjVert[j]];
            y2 = final.projY[final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pAdjVert[j]];
            lineDifGen(x1, y1, x2, y2, final.pCurrentObj->pPolygon[final.polyIdList[i]].R,
                       final.pCurrentObj->pPolygon[final.polyIdList[i]].G,
                       final.pCurrentObj->pPolygon[final.polyIdList[i]].B);
        }
    }
}

void gouraudShade()
{
    int i,ymin, ymax, ymid, xmin, xmax, xmid, zmin, zmid, zmax, y, x, z, dy1, dx1, dy2, dx2, dz1, dz2;
    int xint1, xint2, zint1, zint2,tmp, tmpx1, tmpx2, tmpy1, tmpy2, cR0,cG0,cB0, cR1, cG1, cB1, cR2, cG2, cB2;
    int cRmin, cRmid, cRmax, cGmin, cGmid, cGmax, cBmin, cBmid, cBmax;
    int dry1, dry2, drx, dgy1, dgy2, dgx, dby1, dby2, dbx, crx1, crx2, cgx1, cgx2, cbx1, cbx2;
    float b[3], btmp0, btmp1, Is, Id, Ia, tmpn0, tmpn1, tmpn2, sqrtb;
    register unsigned int tempAddress;

    for(i = 0 ; i < final.seenNumPoly ; i++)
    {
        if((final.pCurrentObj->status & BIT2) != BIT2)
        {
            /* color calculation for Gouraud shading of vertex 0 */
            if(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].n[0] == 0xfffffff)
            {
                tmpn0 = final.pCurrentObj->pPolygon[final.polyIdList[i]].n[0];
                tmpn1 = final.pCurrentObj->pPolygon[final.polyIdList[i]].n[1];
                tmpn2 = final.pCurrentObj->pPolygon[final.polyIdList[i]].n[2];
            }
            else
            {
                tmpn0 = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].n[0];
                tmpn1 = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].n[1];
                tmpn2 = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].n[2];
            }
            btmp0 = sqrt(pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0] -
                    light.pos[0],2) + 
                         pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[1] -
                    light.pos[1],2) +
                         pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[2] -
                    light.pos[2],2));
            btmp1 = sqrt(pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0] -
                    camera.pos[0],2) + 
                         pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[1] -
                    camera.pos[1],2) +
                         pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[2] -
                    camera.pos[2],2));
            b[0] = (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0] -
                    light.pos[0])/btmp0 -
                (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0] -
                    camera.pos[0])/btmp1;
            b[1] = (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[1] -
                    light.pos[1])/btmp0 -
                (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[1] -
                    camera.pos[1])/btmp1;
            b[2] = (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[2] -
                    light.pos[2])/btmp0 -
                (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[2] -
                    camera.pos[2])/btmp1;
            sqrtb =  sqrt(pow(b[0],2) + pow(b[1],2) + pow(b[2],2));
            b[0] = b[0] / sqrtb;
            b[1] = b[1] / sqrtb;
            b[2] = b[2] / sqrtb;
            Ia = 0.6;
            Is = pow((2 * pow((b[0] * tmpn0 +
                               b[1] * tmpn1 +
                               b[2] * tmpn2),2) - 1),40);
            Id = ((light.pos[0] - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0]) 
                  * tmpn0/btmp0 +
                  (light.pos[1] - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[1])
                  * tmpn1/btmp0 +
                  (light.pos[2] - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[2]) 
                  * tmpn2/btmp0);
            if(Id < 0)
            {
                Id = 0;
                Is = 0;
            }

            cR0 = (Ia * (final.pCurrentObj->pPolygon[final.polyIdList[i]].R) +
                (Is + Id * (final.pCurrentObj->pPolygon[final.polyIdList[i]].R)/255) * light.R);
            cG0 = (Ia * final.pCurrentObj->pPolygon[final.polyIdList[i]].G +
                (Is + Id * final.pCurrentObj->pPolygon[final.polyIdList[i]].G/255) * light.G);
            cB0 = (Ia * final.pCurrentObj->pPolygon[final.polyIdList[i]].B +
                (Is + Id * final.pCurrentObj->pPolygon[final.polyIdList[i]].B/255) * light.B);
            
            /* color calculation for Gouraud shading of vertex 1 */
            if(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].n[0] == 0xfffffff)
            {
                tmpn0 = final.pCurrentObj->pPolygon[final.polyIdList[i]].n[0];
                tmpn1 = final.pCurrentObj->pPolygon[final.polyIdList[i]].n[1];
                tmpn2 = final.pCurrentObj->pPolygon[final.polyIdList[i]].n[2];
            }
            else
            {
                tmpn0 = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].n[0];
                tmpn1 = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].n[1];
                tmpn2 = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].n[2];
            }
            btmp0 = sqrt(pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0] -
                    light.pos[0],2) + 
                         pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[1] -
                    light.pos[1],2) +
                         pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[2] -
                    light.pos[2],2));
            btmp1 = sqrt(pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0] -
                    camera.pos[0],2) + 
                         pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[1] -
                    camera.pos[1],2) +
                         pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[2] -
                    camera.pos[2],2));
            b[0] = (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0] -
                    light.pos[0])/btmp0 -
                (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0] -
                    camera.pos[0])/btmp1;
            b[1] = (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[1] -
                    light.pos[1])/btmp0 -
                (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[1] -
                    camera.pos[1])/btmp1;
            b[2] = (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[2] -
                    light.pos[2])/btmp0 -
                (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[2] -
                    camera.pos[2])/btmp1;
            sqrtb =  sqrt(pow(b[0],2) + pow(b[1],2) + pow(b[2],2));
            b[0] = b[0] / sqrtb;
            b[1] = b[1] / sqrtb;
            b[2] = b[2] / sqrtb;
            Is = pow((2 * pow((b[0] * tmpn0 +
                               b[1] * tmpn1 +
                               b[2] * tmpn2),2) - 1),40);
            Id = ((light.pos[0] - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0]) 
                  * tmpn0/btmp0 +
                  (light.pos[1] - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[1])
                  * tmpn1/btmp0 +
                  (light.pos[2] - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[2]) 
                  * tmpn2/btmp0);
            
            if(Id < 0)
            {
                Id = 0;
                Is = 0;
            }
            cR1 = (Ia * (final.pCurrentObj->pPolygon[final.polyIdList[i]].R) +
                (Is + Id * (final.pCurrentObj->pPolygon[final.polyIdList[i]].R)/255) * light.R);
            cG1 = (Ia * final.pCurrentObj->pPolygon[final.polyIdList[i]].G +
                (Is + Id * final.pCurrentObj->pPolygon[final.polyIdList[i]].G/255) * light.G);
            cB1 = (Ia * final.pCurrentObj->pPolygon[final.polyIdList[i]].B +
                (Is + Id * final.pCurrentObj->pPolygon[final.polyIdList[i]].B/255) * light.B);
            
            /* color calculation for Gouraud shading of vertex 2 */
            if(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].n[0] == 0xfffffff)
            {
                tmpn0 = final.pCurrentObj->pPolygon[final.polyIdList[i]].n[0];
                tmpn1 = final.pCurrentObj->pPolygon[final.polyIdList[i]].n[1];
                tmpn2 = final.pCurrentObj->pPolygon[final.polyIdList[i]].n[2];
            }
            else
            {
                tmpn0 = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].n[0];
                tmpn1 = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].n[1];
                tmpn2 = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].n[2];
            }
            btmp0 = sqrt(pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0] -
                    light.pos[0],2) + 
                         pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[1] -
                    light.pos[1],2) +
                         pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[2] -
                    light.pos[2],2));
            btmp1 = sqrt(pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0] -
                    camera.pos[0],2) + 
                         pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[1] -
                    camera.pos[1],2) +
                         pow(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[2] -
                    camera.pos[2],2));
            b[0] = (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0] -
                    light.pos[0])/btmp0 -
                (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0] -
                    camera.pos[0])/btmp1;
            b[1] = (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[1] -
                    light.pos[1])/btmp0 -
                (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[1] -
                    camera.pos[1])/btmp1;
            b[2] = (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[2] -
                    light.pos[2])/btmp0 -
                (final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[2] -
                    camera.pos[2])/btmp1;
            sqrtb =  sqrt(pow(b[0],2) + pow(b[1],2) + pow(b[2],2));
            b[0] = b[0] / sqrtb;
            b[1] = b[1] / sqrtb;
            b[2] = b[2] / sqrtb;
            Is = pow((2 * pow((b[0] * tmpn0 +
                               b[1] * tmpn1 +
                               b[2] * tmpn2),2) - 1),40);
            Id = ((light.pos[0] - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0]) 
                  * tmpn0/btmp0 +
                  (light.pos[1] - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[1])
                  * tmpn1/btmp0 +
                  (light.pos[2] - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[2]) 
                  * tmpn2/btmp0);
            
            if(Id < 0)
            {
                Id = 0;
                Is=0;
            }
            cR2 = (Ia * (final.pCurrentObj->pPolygon[final.polyIdList[i]].R) +
                (Is + Id * (final.pCurrentObj->pPolygon[final.polyIdList[i]].R)/255) * light.R);
            cG2 = (Ia * final.pCurrentObj->pPolygon[final.polyIdList[i]].G +
                (Is + Id * final.pCurrentObj->pPolygon[final.polyIdList[i]].G/255) * light.G);
            cB2 = (Ia * final.pCurrentObj->pPolygon[final.polyIdList[i]].B +
                (Is + Id * final.pCurrentObj->pPolygon[final.polyIdList[i]].B/255) * light.B);

            if(cR0 >= 255)
                cR0 = 255;
            if(cG0 >= 255)
                cG0 = 255;
            if(cB0 >= 255)
                cB0 = 255;
            if(cR1 >= 255)
                cR1 = 255;
            if(cG1 >= 255)
                cG1 = 255;
            if(cB1 >= 255)
                cB1 = 255;
            if(cR2 >= 255)
                cR2 = 255;
            if(cG2 >= 255)
                cG2 = 255;
            if(cB2 >= 255)
                cB2 = 255;
            if(cR0 < 0)
                cR0 = 0;
            if(cG0 < 0)
                cG0 = 0;
            if(cB0 < 0)
                cB0 = 0;
            if(cR1 < 0)
                cR1 = 0;
            if(cG1 < 0)
                cG1 = 0;
            if(cB1 < 0)
                cB1 = 0;
            if(cR2 < 0)
                cR2 = 0;
            if(cG2 < 0)
                cG2 = 0;
            if(cB2 < 0)
                cB2 = 0;
        }
        else if ((final.pCurrentObj->status & BIT2) == BIT2)
        {
            cR0 = final.pCurrentObj->pPolygon[final.polyIdList[i]].R;
            cG0 = final.pCurrentObj->pPolygon[final.polyIdList[i]].G;
            cB0 = final.pCurrentObj->pPolygon[final.polyIdList[i]].B;
        }
        if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] <= final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] &&
           final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] <= final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]])
        {
                ymin = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                xmin = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                zmin = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                cRmin = cR0;
                cGmin = cG0;
                cBmin = cB0;
                if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] < final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]])
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    cRmid = cR1;
                    cGmid = cG1;
                    cBmid = cB1;
                    cRmax = cR2;
                    cGmax = cG2;
                    cBmax = cB2;
                }
                else
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                    cRmid = cR2;
                    cGmid = cG2;
                    cBmid = cB2;
                    cRmax = cR1;
                    cGmax = cG1;
                    cBmax = cB1;
                }
        }
        else if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] <= final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] &&
           final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] <= final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]])
        {
                ymin = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                xmin = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                zmin = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                cRmin = cR1;
                cGmin = cG1;
                cBmin = cB1;

                if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] < final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]])
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                    cRmid = cR0;
                    cGmid = cG0;
                    cBmid = cB0;
                    cRmax = cR2;
                    cGmax = cG2;
                    cBmax = cB2;
                }
                else
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                    cRmid = cR2;
                    cGmid = cG2;
                    cBmid = cB2;
                    cRmax = cR0;
                    cGmax = cG0;
                    cBmax = cB0;
                }
        }
        else
        {
                ymin = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                xmin = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                zmin = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                cRmin = cR2;
                cGmin = cG2;
                cBmin = cB2;

                if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] < final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]])
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                    cRmid = cR1;
                    cGmid = cG1;
                    cBmid = cB1;
                    cRmax = cR0;
                    cGmax = cG0;
                    cBmax = cB0;
                }
                else
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                    cRmid = cR0;
                    cGmid = cG0;
                    cBmid = cB0;
                    cRmax = cR1;
                    cGmax = cG1;
                    cBmax = cB1;
                }
        }
        if(ymax < 0 || ymin > VERTICAL || (xmin < 0 && xmid < 0 && xmax < 0) ||
           (xmin > HORIZONTAL && xmid > HORIZONTAL && xmax > HORIZONTAL))
           continue;


        dy1 = ymid - ymin;
        dx1 = xmid - xmin;
        dz1 = zmid - zmin;
        dry1 = cRmid - cRmin;
        dgy1 = cGmid - cGmin;
        dby1 = cBmid - cBmin;
        dy2 = ymax - ymin;
        dx2 = xmax - xmin;
        dz2 = zmax - zmin;
        dry2 = cRmax - cRmin;
        dgy2 = cGmax - cGmin;
        dby2 = cBmax - cBmin;
        if(ymin < 0)
            tmpy1 = 0;
        else if( ymin > VERTICAL)
            tmpy1 = VERTICAL - 2;
        else
            tmpy1 = ymin;
        if(ymid < 0)
            tmpy2 = 0;
        else if( ymid > VERTICAL)
            tmpy2 = VERTICAL - 2;
        else
            tmpy2 = ymid;
        for(y = tmpy1 ; y < tmpy2 ; y++)
        {
            xint1 = xmin + (y - ymin)*dx1/dy1;
            xint2 = xmin + (y - ymin)*dx2/dy2;
            if((final.pCurrentObj->status & BIT2) != BIT2)
            {
                zint1 = zmin + (y - ymin)*dz1/dy1;
                zint2 = zmin + (y - ymin)*dz2/dy2;
                crx1 = cRmin + (y - ymin)*dry1/dy1;
                crx2 = cRmin + (y - ymin)*dry2/dy2;
                cgx1 = cGmin + (y - ymin)*dgy1/dy1;
                cgx2 = cGmin + (y - ymin)*dgy2/dy2;
                cbx1 = cBmin + (y - ymin)*dby1/dy1;
                cbx2 = cBmin + (y - ymin)*dby2/dy2;
            }
            if(xint1 > xint2)
            {
                tmp = xint1;
                xint1 = xint2;
                xint2 = tmp;
                tmp = crx1;
                crx1= crx2;
                crx2 = tmp;
                tmp = cgx1;
                cgx1= cgx2;
                cgx2 = tmp;
                tmp = cbx1;
                cbx1= cbx2;
                cbx2 = tmp;
                tmp = zint1;
                zint1 = zint2;
                zint2 = tmp;
            }
            drx = (crx2 - crx1);
            dgx = (cgx2 - cgx1);
            dbx = (cbx2 - cbx1);
            if(xint1 < 0)
                tmpx1 = 0;
            else if( xint1 > HORIZONTAL)
                tmpx1 = HORIZONTAL - 2;
            else
                tmpx1 = xint1;                                                    
            if(xint2 < 0)
                tmpx2 = 0;
            else if( xint2 > HORIZONTAL)
                tmpx2 = HORIZONTAL - 2;
            else
                tmpx2 = xint2;
            for(x = tmpx1 ; x <= tmpx2 ; x++)
            {
                if((final.pCurrentObj->status & BIT2) != BIT2)
                {
                    z = zint1 + (x - xint1) * (zint2 - zint1)/(xint2 - xint1);
                }
                if(GT_READWORD(zBuffer + x + (y * HORIZONTAL)) >= z && (final.pCurrentObj->status & BIT2) != BIT2)
                {
                    tempAddress = frameBuffer + x*3 + (y << 11)/*(y*2048)*/;
                    GT_WRITE_CHAR(tempAddress,
                                  (int)(cbx1 + dbx * (x - xint1)/(xint2 - xint1)));
                    GT_WRITE_CHAR(tempAddress + 1,
                                  (int)(cgx1 + dgx * (x - xint1)/(xint2 - xint1)));
                    GT_WRITE_CHAR(tempAddress + 2,
                                  ((int)(crx1 + drx * (x - xint1)/(xint2 - xint1))));
                    GT_WRITE_WORD(zBuffer + x + (y * HORIZONTAL) , z);
                }
                else if((final.pCurrentObj->status & BIT2) == BIT2)
                {
                    tempAddress = frameBuffer + x*3 + (y << 11)/*(y*2048)*/;
                    GT_WRITE_CHAR(tempAddress    ,cB0);
                    GT_WRITE_CHAR(tempAddress + 1,cG0);
                    GT_WRITE_CHAR(tempAddress + 2,cR0);
                }
            }
        }
        dy1 = ymax - ymid;
        dx1 = xmax - xmid;
        dz1 = zmax - zmid;
        dry1 = cRmax - cRmid;
        dgy1 = cGmax - cGmid;
        dby1 = cBmax - cBmid;
        if(ymax < 0)
            tmpy1 = 0;
        else if( ymax > VERTICAL)
            tmpy1 = VERTICAL - 2;
        else
            tmpy1 = ymax;
        for(y = tmpy2 ; y < tmpy1 ; y++)
        {
            crx1 = cRmid + (y - ymid)*dry1/dy1;
            crx2 = cRmin + (y - ymin)*dry2/dy2;
            cgx1 = cGmid + (y - ymid)*dgy1/dy1;
            cgx2 = cGmin + (y - ymin)*dgy2/dy2;
            cbx1 = cBmid + (y - ymid)*dby1/dy1;
            cbx2 = cBmin + (y - ymin)*dby2/dy2;
            xint1 = xmid + (y - ymid)*dx1/dy1;
            xint2 = xmin + (y - ymin)*dx2/dy2;
            zint1 = zmid + (y - ymid)*dz1/dy1;
            zint2 = zmin + (y - ymin)*dz2/dy2;
            if(xint1 > xint2)
            {
                tmp = xint1;
                xint1 = xint2;
                xint2 = tmp;
                tmp = crx1;
                crx1= crx2;
                crx2 = tmp;
                tmp = cgx1;
                cgx1= cgx2;
                cgx2 = tmp;
                tmp = cbx1;
                cbx1= cbx2;
                cbx2 = tmp;
                tmp = zint1;
                zint1 = zint2;
                zint2 = tmp;
            }
            drx = (crx2 - crx1);
            dgx = (cgx2 - cgx1);
            dbx = (cbx2 - cbx1);
            if(xint1 < 0)
                tmpx1 = 0;
            else if( xint1 > HORIZONTAL)
                tmpx1 = HORIZONTAL - 2;
            else
                tmpx1 = xint1;
            if(xint2 < 0)
                tmpx2 = 0;
            else if( xint2 > HORIZONTAL)
                tmpx2 = HORIZONTAL - 2;
            else
                tmpx2 = xint2;
            for(x = tmpx1 ; x <= tmpx2 ; x++)
            {
                if((final.pCurrentObj->status & BIT2) != BIT2)
                {
                    z = zint1 + (x - xint1) * (zint2 - zint1)/(xint2 - xint1);
                }
                if(GT_READWORD(zBuffer + x + (y * HORIZONTAL)) >= z && (final.pCurrentObj->status & BIT2) != BIT2)
                {
                    tempAddress = frameBuffer + x*3 + (y << 11)/*(y*2048)*/;
                    GT_WRITE_CHAR(tempAddress,
                                  (int)(cbx1 + dbx * (x - xint1)/(xint2 - xint1)));
                    GT_WRITE_CHAR(tempAddress + 1,
                                  (int)(cgx1 + dgx * (x - xint1)/(xint2 - xint1)));
                    GT_WRITE_CHAR(tempAddress + 2,
                                  ((int)(crx1 + drx * (x - xint1)/(xint2 - xint1))));
                    GT_WRITE_WORD(zBuffer + x + (y * HORIZONTAL) , z);
                }
                else if((final.pCurrentObj->status & BIT2) == BIT2)
                {
                    tempAddress = frameBuffer + x*3 + (y << 11)/*(y*2048)*/;
                    GT_WRITE_CHAR(tempAddress    ,cB0);
                    GT_WRITE_CHAR(tempAddress + 1,cG0);
                    GT_WRITE_CHAR(tempAddress + 2,cR0);
                }

            }
        }
    } 
}

void flatShade(int shadowFlag)
{
    int i,ymin, ymax, ymid, xmin, xmax, xmid, zmin, zmid, zmax, y, x, z, dy1, dx1, dy2, dx2, dz1, dz2;
    int xint1, xint2, zint1, zint2, tmp, tmpx1, tmpx2, tmpy1, tmpy2;
    float btmp0, btmp1,b[3],Ia,Is,Id,tmpn0,tmpn1,tmpn2,sqrtb;
    int cR0,cG0,cB0;
    for(i = 0 ; i < final.seenNumPoly ; i++)
    {
        if(shadowFlag == 0 && ((final.pCurrentObj->status & BIT2) != BIT2))
        {
            
            tmpn0 = final.pCurrentObj->pPolygon[final.polyIdList[i]].n[0];
            tmpn1 = final.pCurrentObj->pPolygon[final.polyIdList[i]].n[1];
            tmpn2 = final.pCurrentObj->pPolygon[final.polyIdList[i]].n[2];


            btmp0 = sqrt((final.pCurrentObj->pos[0] - light.pos[0])*(final.pCurrentObj->pos[0] - light.pos[0]) + 
                         (final.pCurrentObj->pos[1] - light.pos[1])*(final.pCurrentObj->pos[1] - light.pos[1]) +
                         (final.pCurrentObj->pos[2] - light.pos[2])*(final.pCurrentObj->pos[2] - light.pos[2]));
            btmp1 = sqrt((final.pCurrentObj->pos[0] - camera.pos[0])*(final.pCurrentObj->pos[0] - camera.pos[0]) + 
                         (final.pCurrentObj->pos[1] - camera.pos[1])*(final.pCurrentObj->pos[1] - camera.pos[1]) +
                         (final.pCurrentObj->pos[2] - camera.pos[2])*(final.pCurrentObj->pos[2] - camera.pos[2]));
            b[0] = (final.pCurrentObj->pos[0] - light.pos[0])/btmp0 -
                (final.pCurrentObj->pos[0] - camera.pos[0])/btmp1;
            b[1] = (final.pCurrentObj->pos[1] - light.pos[1])/btmp0 -
                (final.pCurrentObj->pos[1] - camera.pos[1])/btmp1;
            b[2] = (final.pCurrentObj->pos[2] - light.pos[2])/btmp0 -
                (final.pCurrentObj->pos[2] - camera.pos[2])/btmp1;
            sqrtb =  sqrt(b[0]*b[0] + b[1]*b[1] + b[2]*b[2]);
            b[0] = b[0] / sqrtb;
            b[1] = b[1] / sqrtb;
            b[2] = b[2] / sqrtb;
            Ia = 0.6;
            Is = pow((2 * ((b[0] * tmpn0 + b[1] * tmpn1 + b[2] * tmpn2)*(b[0] * tmpn0 + b[1] * tmpn1 + b[2] * tmpn2)) - 1),20);
            Id = ((light.pos[0] - final.pCurrentObj->pos[0]) * tmpn0/btmp0 +
                  (light.pos[1] - final.pCurrentObj->pos[1]) * tmpn1/btmp0 +
                  (light.pos[2] - final.pCurrentObj->pos[2]) * tmpn2/btmp0);
            cR0 = (Ia * (final.pCurrentObj->pPolygon[final.polyIdList[i]].R) +
                (Is + Id * (final.pCurrentObj->pPolygon[final.polyIdList[i]].R)/255) * light.R);
            cG0 = (Ia * final.pCurrentObj->pPolygon[final.polyIdList[i]].G +
                (Is + Id * final.pCurrentObj->pPolygon[final.polyIdList[i]].G/255) * light.G);
            cB0 = (Ia * final.pCurrentObj->pPolygon[final.polyIdList[i]].B +
                (Is + Id * final.pCurrentObj->pPolygon[final.polyIdList[i]].B/255) * light.B);
        }
        else if(shadowFlag == 1 && (final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0] == 0xfffffff ||
                final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0] == 0xfffffff ||
                                    final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0] == 0xfffffff))
            continue;
        else if ((final.pCurrentObj->status & BIT2) == BIT2)
        {
            cR0 = final.pCurrentObj->pPolygon[final.polyIdList[i]].R;
            cG0 = final.pCurrentObj->pPolygon[final.polyIdList[i]].G;
            cB0 = final.pCurrentObj->pPolygon[final.polyIdList[i]].B;
        }
        if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] <= final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] &&
           final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] <= final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]])
        {
                ymin = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                xmin = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                zmin = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] < final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]])
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                }
                else
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                }
        }
        else if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] <= final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] &&
           final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] <= final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]])
        {
                ymin = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                xmin = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                zmin = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] < final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]])
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                }
                else
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                }
        }
        else
        {
                ymin = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                xmin = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                zmin = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] < final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]])
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                }
                else
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                }
        }
        if(cR0 >= 255)
            cR0 = 255;
        if(cG0 >= 255)
            cG0 = 255;
        if(cB0 >= 255)
            cB0 = 255;
        if(cR0 < 0)
            cR0 = 0;
        if(cG0 < 0)
            cG0 = 0;
        if(cB0 < 0)
            cB0 = 0;
        
        dy1 = ymid - ymin;
        dx1 = xmid - xmin;
        dz1 = zmid - zmin;
        dy2 = ymax - ymin;
        dx2 = xmax - xmin;
        dz2 = zmax - zmin;
        if(ymin < 0)
            tmpy1 = 0;
        else if( ymin > VERTICAL)
            tmpy1 = VERTICAL;
        else
            tmpy1 = ymin;
        if(ymid < 0)
            tmpy2 = 0;
        else if( ymid > VERTICAL)
            tmpy2 = VERTICAL;
        else
            tmpy2 = ymid;
        for(y = tmpy1 ; y < tmpy2 ; y++)
        {
            xint1 = xmin + (y - ymin)*dx1/dy1;
            xint2 = xmin + (y - ymin)*dx2/dy2;
            if((final.pCurrentObj->status & BIT2) != BIT2)
            {
                zint1 = zmin + (y - ymin)*dz1/dy1;
                zint2 = zmin + (y - ymin)*dz2/dy2;
            }
            if(xint1 > xint2)
            {
                tmp = xint1;
                xint1 = xint2;
                xint2 = tmp;
                tmp = zint1;
                zint1 = zint2;
                zint2 = tmp;
            }
            if(xint1 < 0)
                tmpx1 = 2;
            else if( xint1 > HORIZONTAL)
                tmpx1 = HORIZONTAL - 2;
            else
                tmpx1 = xint1;
            if(xint2 < 0)
                tmpx2 = 2;
            else if( xint2 > HORIZONTAL)
                tmpx2 = HORIZONTAL - 2;
            else
                tmpx2 = xint2;
            tmp = tmpx2 - tmpx1;
            for(x = tmpx1 ; x < tmpx2 ; x++)
            {
                if((final.pCurrentObj->status & BIT2) != BIT2)
                {
                    z = zint1 + (x - xint1) * (zint2 - zint1)/(xint2 - xint1);
                }
                if(GT_READWORD(zBuffer + x + (y * HORIZONTAL)) >= z && (final.pCurrentObj->status & BIT2) != BIT2)
                {
                    if(shadowFlag == 0)
                    {
                        GT_WRITE_CHAR((frameBuffer + x * 3 + (y << 11)/*(y*2048)*/),cB0);
                        GT_WRITE_CHAR((frameBuffer + x * 3 + 1 + (y << 11)/*(y*2048)*/),cG0);
                        GT_WRITE_CHAR((frameBuffer + x * 3 + 2 + (y << 11)/*(y*2048)*/),cR0);
                    }
                    else
                    {
                        GT_WRITE_CHAR((frameBuffer + x * 3 + (y << 11)/*(y*2048)*/),80);
                        GT_WRITE_CHAR((frameBuffer + x * 3 + 1 + (y << 11)/*(y*2048)*/),80);
                        GT_WRITE_CHAR((frameBuffer + x * 3 + 2 + (y << 11)/*(y*2048)*/),80);
                    }
                    GT_WRITE_WORD(zBuffer + x + (y * HORIZONTAL) , z);
                }
                else if((final.pCurrentObj->status & BIT2) == BIT2)
                {   
                    if((tmp > 32) && (x > (tmpx1 + 10)) && (((unsigned int)frameBuffer + x*3 + (y << 11)/*(y*2048)*/) % 24)==0)
                    {
                        while(gtDmaIsChannelActive(DMA_ENG_3));
                        gtDmaTransfer(DMA_ENG_3,VIRTUAL_TO_PHY(frameBuffer) + tmpx1*3 + (y << 11)/*(y*2048)*/+8-((unsigned int)(VIRTUAL_TO_PHY(frameBuffer)) + tmpx1*3 + (y << 11)/*(y*2048)*/)%8,
                                      VIRTUAL_TO_PHY(frameBuffer) + x*3 + (y << 11)/*(y*2048)*/, (tmpx2 - x)*3,
                                      DMA_DTL_32BYTES | DMA_BLOCK_TRANSFER_MODE
                                      | DMA_HOLD_SOURCE_ADDR, NULL);
                        break;
                    }
                    {
                        GT_WRITE_CHAR((frameBuffer + x * 3 + (y << 11)/*(y*2048)*/),cB0);
                        GT_WRITE_CHAR((frameBuffer + x * 3 + 1 + (y << 11)/*(y*2048)*/),cG0);
                        GT_WRITE_CHAR((frameBuffer + x * 3 + 2 + (y << 11)/*(y*2048)*/),cR0);
                    }
                }
            }
        }
        dy1 = ymax - ymid;
        dx1 = xmax - xmid;
        dz1 = zmax - zmid;
        if(ymax < 0)
            tmpy1 = 0;
        else if( ymax > VERTICAL)
            tmpy1 = VERTICAL;
        else
            tmpy1 = ymax;
        for(y = tmpy2 ; y < tmpy1 ; y++)
        {
            xint1 = xmid + (y - ymid)*dx1/dy1;
            xint2 = xmin + (y - ymin)*dx2/dy2;
            zint1 = zmid + (y - ymid)*dz1/dy1;
            zint2 = zmin + (y - ymin)*dz2/dy2;
            if(xint1 > xint2)
            {
                tmp = xint1;
                xint1 = xint2;
                xint2 = tmp;
                tmp = zint1;
                zint1 = zint2;
                zint2 = tmp;
            }
            if(xint1 < 0)
                tmpx1 = 2;
            else if( xint1 > HORIZONTAL)
                tmpx1 = HORIZONTAL - 2;
            else
                tmpx1 = xint1;
            if(xint2 < 0)
                tmpx2 = 2;
            else if( xint2 > HORIZONTAL)
                tmpx2 = HORIZONTAL - 2;
            else
                tmpx2 = xint2;
            tmp = tmpx2 - tmpx1;
            for(x = tmpx1 ; x < tmpx2 ; x++)
            {
                z = zint1 + (x - xint1) * (zint2 - zint1)/(xint2 - xint1);
                if(GT_READWORD(zBuffer + x + (y * HORIZONTAL)) >= z && (final.pCurrentObj->status & BIT2) != BIT2)
                {
                    if(shadowFlag == 0)
                    {
                        GT_WRITE_CHAR((frameBuffer + x * 3 + (y << 11)/*(y*2048)*/),cB0);
                        GT_WRITE_CHAR((frameBuffer + x * 3 + 1 + (y << 11)/*(y*2048)*/),cG0);
                        GT_WRITE_CHAR((frameBuffer + x * 3 + 2 + (y << 11)/*(y*2048)*/),cR0);
                    }
                    else
                    {
                        GT_WRITE_CHAR((frameBuffer + x * 3 + (y << 11)/*(y*2048)*/),80);
                        GT_WRITE_CHAR((frameBuffer + x * 3 + 1 + (y << 11)/*(y*2048)*/),80);
                        GT_WRITE_CHAR((frameBuffer + x * 3 + 2 + (y << 11)/*(y*2048)*/),80);
                    }
                    GT_WRITE_WORD(zBuffer + x + (y * HORIZONTAL) , z);
                }
                else if((final.pCurrentObj->status & BIT2) == BIT2)
                {
                    if((tmp > 32) && (x > (tmpx1 + 10)) && (((unsigned int)frameBuffer + x*3 + (y << 11)/*(y*2048)*/) % 24)==0)
                    {
                        while(gtDmaIsChannelActive(DMA_ENG_3));
                        gtDmaTransfer(DMA_ENG_3,VIRTUAL_TO_PHY(frameBuffer) + tmpx1*3 + (y << 11)/*(y*2048)*/+8-((unsigned int)(VIRTUAL_TO_PHY(frameBuffer)) + tmpx1*3 + (y << 11)/*(y*2048)*/)%8,
                                      VIRTUAL_TO_PHY(frameBuffer) + x*3 + (y << 11)/*(y*2048)*/, (tmpx2 - x)*3,
                                      DMA_DTL_32BYTES | DMA_BLOCK_TRANSFER_MODE
                                      | DMA_HOLD_SOURCE_ADDR, NULL);
                        break;
                    }
                    {
                        GT_WRITE_CHAR((frameBuffer + x * 3 + (y << 11)/*(y*2048)*/),cB0);
                        GT_WRITE_CHAR((frameBuffer + x * 3 + 1 + (y << 11)/*(y*2048)*/),cG0);
                        GT_WRITE_CHAR((frameBuffer + x * 3 + 2 + (y << 11)/*(y*2048)*/),cR0);
                    }
                }
            }
        }  
    } 
}

int loadObjects(OBJECT ** object, char * fileName, int * numObjects)
{
    OBJECT * obj;
    FS_FILE_HANDLER file;
    char buf[100],tmp;
    int i= 0, j, k, l, numAdj, numAdjPoly, v1[3], v2[3], length;
    int adjVertices[100], adjPolygons[100], tmp1, tmp2;

    if((file = gtFSOpen(fileName,"r"))== FS_FILE_OPEN_ERROR)
    {
        printf("%s file not found\n",fileName);
        return 0;
    }
    while(1)
    {
        gtFSRead(file,1,&tmp);
        if(tmp =='\n')
            break;
        buf[i++] = tmp;
    }
    buf[i] = '\0';
    sscanf(buf,"%d",numObjects);
    obj = (OBJECT *) malloc(sizeof(OBJECT) * (*numObjects));
    *object = obj;
    for(i = obj ; i < ((unsigned int)obj + sizeof(OBJECT) * (*numObjects)) ; i+=4)
        *(unsigned int *)i = 0;
    for(j = 0; j < (*numObjects) ; j++)
    {
        i= 0;
        obj[j].status = BIT0;
        while(1)
        {
            gtFSRead(file,1,&tmp);
            if(tmp =='\n')
                break;
            buf[i++] = tmp;
        }
        buf[i] = '\0';
        sscanf(buf,"%d", &obj[j].numPolygons);
        obj[j].pPolygon = (POLYGON *)malloc(sizeof(POLYGON) * obj[j].numPolygons);
        for(i = (unsigned int)obj[j].pPolygon ; i < ((unsigned int)obj[j].pPolygon + (sizeof(POLYGON) * obj[j].numPolygons)) ; i+=4)
        {
            *(unsigned int *)i = 0;
        }
        for(k = 0 ; k < obj[j].numPolygons ; k++)
        {
            i= 0;
            while(1)
            {
                gtFSRead(file,1,&tmp);
                if(tmp =='\n')
                    break;
                buf[i++] = tmp;
            }
            buf[i] = '\0';
            sscanf(buf,"%d %d %d %d %d %d", &obj[j].pPolygon[k].vertexID[0], &obj[j].pPolygon[k].vertexID[1],
                   &obj[j].pPolygon[k].vertexID[2], &obj[j].pPolygon[k].R, &obj[j].pPolygon[k].G,
                   &obj[j].pPolygon[k].B);
        }
        i= 0;
        while(1)
        {
            gtFSRead(file,1,&tmp);
            if(tmp =='\n')
                break;
            buf[i++] = tmp;
        }
        buf[i] = '\0';
        sscanf(buf,"%d", &obj[j].numVeritces);
        obj[j].pVertex = (VERTEX *)malloc(sizeof(VERTEX) * obj[j].numVeritces);
        for(i = (unsigned int)obj[j].pVertex ; i < ((unsigned int)obj[j].pVertex + sizeof(VERTEX) * obj[j].numVeritces) ; i+=4)
            *(unsigned int *)i = 0;
        obj[j].pos[0] = 0;
        obj[j].pos[1] = 0;
        obj[j].pos[2] = 0;
        for(k = 0 ; k < obj[j].numVeritces ; k++)
        {
            i= 0;
            while(1)
            {
                gtFSRead(file,1,&tmp);
                if(tmp =='\n')
                    break;
                buf[i++] = tmp;
            }
            buf[i] = '\0';
            sscanf(buf,"%f %f %f", &obj[j].pVertex[k].pos[0], &obj[j].pVertex[k].pos[1],
                   &obj[j].pVertex[k].pos[2]);
            obj[j].pos[0]+= obj[j].pVertex[k].pos[0];
            obj[j].pos[1]+= obj[j].pVertex[k].pos[1];
            obj[j].pos[2]+= obj[j].pVertex[k].pos[2];
        }

         obj[j].pos[0]/= obj[j].numVeritces;
         obj[j].pos[1]/= obj[j].numVeritces;
         obj[j].pos[2]/= obj[j].numVeritces;
        /* now let's find for each polygon the adjacent polygons */
        for(i = 0 ; i < obj[j].numPolygons ; i++)
        {
            for(k = 0 ; k < obj[j].numPolygons ; k++)
            {
                if((obj[j].pPolygon[i].vertexID[0] == obj[j].pPolygon[k].vertexID[0] ||
                    obj[j].pPolygon[i].vertexID[0] == obj[j].pPolygon[k].vertexID[1] ||
                    obj[j].pPolygon[i].vertexID[0] == obj[j].pPolygon[k].vertexID[2]) &&
                   (obj[j].pPolygon[i].vertexID[1] == obj[j].pPolygon[k].vertexID[0] ||
                    obj[j].pPolygon[i].vertexID[1] == obj[j].pPolygon[k].vertexID[1] ||
                    obj[j].pPolygon[i].vertexID[1] == obj[j].pPolygon[k].vertexID[2]) && (k != i))
                    obj[j].pPolygon[i].AdjPoly[0] = k;
                else if((obj[j].pPolygon[i].vertexID[1] == obj[j].pPolygon[k].vertexID[0] ||
                        obj[j].pPolygon[i].vertexID[1] == obj[j].pPolygon[k].vertexID[1] ||
                        obj[j].pPolygon[i].vertexID[1] == obj[j].pPolygon[k].vertexID[2]) &&
                       (obj[j].pPolygon[i].vertexID[2] == obj[j].pPolygon[k].vertexID[0] ||
                        obj[j].pPolygon[i].vertexID[2] == obj[j].pPolygon[k].vertexID[1] ||
                        obj[j].pPolygon[i].vertexID[2] == obj[j].pPolygon[k].vertexID[2]) && (k != i))
                        obj[j].pPolygon[i].AdjPoly[1] = k;
                else if((obj[j].pPolygon[i].vertexID[2] == obj[j].pPolygon[k].vertexID[0] ||
                        obj[j].pPolygon[i].vertexID[2] == obj[j].pPolygon[k].vertexID[1] ||
                        obj[j].pPolygon[i].vertexID[2] == obj[j].pPolygon[k].vertexID[2]) &&
                       (obj[j].pPolygon[i].vertexID[0] == obj[j].pPolygon[k].vertexID[0] ||
                        obj[j].pPolygon[i].vertexID[0] == obj[j].pPolygon[k].vertexID[1] ||
                        obj[j].pPolygon[i].vertexID[0] == obj[j].pPolygon[k].vertexID[2]) && (k != i))
                        obj[j].pPolygon[i].AdjPoly[2] = k;
            }
        }
        /* now let's find for each vertex the adjacent vertices and polygons*/
        for(i = 0 ; i < obj[j].numVeritces ; i++)
        {     
            numAdj = numAdjPoly = 0;
            for(k = 0 ; k < obj[j].numPolygons ; k++)
            {
                tmp1 = tmp2 = 0xfffffff;
                if(obj[j].pPolygon[k].vertexID[0] == i)
                {
                    tmp1 = obj[j].pPolygon[k].vertexID[1];
                    tmp2 = obj[j].pPolygon[k].vertexID[2];
                    adjPolygons[numAdjPoly++] = k;
                }
                else if(obj[j].pPolygon[k].vertexID[1] == i)
                {
                    tmp1 = obj[j].pPolygon[k].vertexID[0];
                    tmp2 = obj[j].pPolygon[k].vertexID[2];
                    adjPolygons[numAdjPoly++] = k;
                }
                else if(obj[j].pPolygon[k].vertexID[2] == i)
                {
                    tmp1 = obj[j].pPolygon[k].vertexID[1];
                    tmp2 = obj[j].pPolygon[k].vertexID[0];
                    adjPolygons[numAdjPoly++] = k;
                }
                else
                    continue;
                for(l = 0 ; l < numAdj ; l++)
                {
                    if(tmp1 == adjVertices[l])
                        tmp1 = 0xfffffff;
                    if(tmp2 == adjVertices[l])
                        tmp2 = 0xfffffff;
                }

                if(tmp1 != 0xfffffff)
                {
                    for(l = 0 ; l < obj[j].pVertex[tmp1].numAdjVert ; l++)
                    {
                        if(i == obj[j].pVertex[tmp1].pAdjVert[l])
                        {
                            tmp1=0xfffffff;
                            break;
                        }
                    }
                    if(tmp1 != 0xfffffff)
                        adjVertices[numAdj++] = tmp1;
                }
                if(tmp2 != 0xfffffff)
                {
                    for(l = 0 ; l < obj[j].pVertex[tmp2].numAdjVert ; l++)
                    {
                        if(i == obj[j].pVertex[tmp2].pAdjVert[l])
                        {
                            tmp2=0xfffffff;
                            break;
                        }
                    }
                    if(tmp2 != 0xfffffff)
                        adjVertices[numAdj++] = tmp2;
                }
            }
            obj[j].pVertex[i].pAdjVert = (unsigned int *)malloc(4 * numAdj);
            obj[j].pVertex[i].numAdjVert = numAdj;
            obj[j].pVertex[i].pAdjPoly = (unsigned int *)malloc(4 * numAdjPoly);
            obj[j].pVertex[i].numAdjPoly = numAdjPoly;
            for(l = 0 ; l < numAdj ; l++)
            {
                obj[j].pVertex[i].pAdjVert[l] = adjVertices[l];
            }
            for(l = 0 ; l < numAdjPoly ; l++)
                obj[j].pVertex[i].pAdjPoly[l] = adjPolygons[l];
        }

        /* normal = (0-1) x (2-1) */
        for(i = 0 ; i < obj[j].numPolygons ; i++)
        {
            v1[0] = obj[j].pVertex[obj[j].pPolygon[i].vertexID[0]].pos[0] -
                obj[j].pVertex[obj[j].pPolygon[i].vertexID[1]].pos[0];
            v1[1] = obj[j].pVertex[obj[j].pPolygon[i].vertexID[0]].pos[1] - 
                obj[j].pVertex[obj[j].pPolygon[i].vertexID[1]].pos[1];
            v1[2] = obj[j].pVertex[obj[j].pPolygon[i].vertexID[0]].pos[2] - 
                obj[j].pVertex[obj[j].pPolygon[i].vertexID[1]].pos[2];
            v2[0] = obj[j].pVertex[obj[j].pPolygon[i].vertexID[2]].pos[0] -
                obj[j].pVertex[obj[j].pPolygon[i].vertexID[1]].pos[0];
            v2[1] = obj[j].pVertex[obj[j].pPolygon[i].vertexID[2]].pos[1] - 
                obj[j].pVertex[obj[j].pPolygon[i].vertexID[1]].pos[1];
            v2[2] = obj[j].pVertex[obj[j].pPolygon[i].vertexID[2]].pos[2] - 
                obj[j].pVertex[obj[j].pPolygon[i].vertexID[1]].pos[2];
            obj[j].pPolygon[i].n[0] = v1[1] * v2[2] - v1[2] * v2[1];
            obj[j].pPolygon[i].n[1] = v1[2] * v2[0] - v1[0] * v2[2];
            obj[j].pPolygon[i].n[2] = v1[0] * v2[1] - v1[1] * v2[0];
            length = sqrt(pow(obj[j].pPolygon[i].n[0],2)+pow(obj[j].pPolygon[i].n[1],2)+
                          pow(obj[j].pPolygon[i].n[2],2));
            if(length==0)
                length = 1;
            obj[j].pPolygon[i].n[0]/=length;
            obj[j].pPolygon[i].n[1]/=length;
            obj[j].pPolygon[i].n[2]/=length;
        }
        
        /* find the vertex normal from the average polygon normal*/
        for(i = 0 ; i < obj[j].numVeritces ; i++)
        {
            int flag;

            flag = 1;
            obj[j].pVertex[i].n[0] = 0;
            obj[j].pVertex[i].n[1] = 0;
            obj[j].pVertex[i].n[2] = 0;
            for(k = 0 ; k < obj[j].pVertex[i].numAdjPoly ; k++)
            {
                if((k > 0) &&
                   fabs(obj[j].pVertex[i].n[0] * obj[j].pPolygon[obj[j].pVertex[i].pAdjPoly[k]].n[0]/k +
                    obj[j].pVertex[i].n[1] * obj[j].pPolygon[obj[j].pVertex[i].pAdjPoly[k]].n[1]/k +
                    obj[j].pVertex[i].n[2] * obj[j].pPolygon[obj[j].pVertex[i].pAdjPoly[k]].n[2]/k) < gcos(45))
                {
                    flag = 0;
                    break;
                }
                obj[j].pVertex[i].n[0] += obj[j].pPolygon[obj[j].pVertex[i].pAdjPoly[k]].n[0];
                obj[j].pVertex[i].n[1] += obj[j].pPolygon[obj[j].pVertex[i].pAdjPoly[k]].n[1];
                obj[j].pVertex[i].n[2] += obj[j].pPolygon[obj[j].pVertex[i].pAdjPoly[k]].n[2];
            }
            if(flag)
            {
                obj[j].pVertex[i].n[0] /= obj[j].pVertex[i].numAdjPoly;
                obj[j].pVertex[i].n[1] /= obj[j].pVertex[i].numAdjPoly;
                obj[j].pVertex[i].n[2] /= obj[j].pVertex[i].numAdjPoly;
            }
            else
            {
                obj[j].pVertex[i].n[0] = 0xfffffff;
                obj[j].pVertex[i].n[1] = 0xfffffff;
                obj[j].pVertex[i].n[2] = 0xfffffff;
            }
        }
    }
    gtFSClose(file);
    return 1;
}

void copyObject(OBJECT ** dest, OBJECT * source, int * numObject)
{
    unsigned int i, j, k,numObjects;
    OBJECT * tmp;
    numObjects = *numObject;
    numObjects++;
    *numObject= numObjects;
    tmp = (OBJECT *) malloc(sizeof(OBJECT) * (numObjects));
    j = (unsigned int)*dest;
    k = (unsigned int)tmp;
    for(i = 0 ; i < ((numObjects) * sizeof(OBJECT))-sizeof(OBJECT) ; i+=4)
    {    
        *(volatile unsigned int *)(k + i)= *(volatile unsigned int *)(j + i);
    }
    
    tmp[numObjects - 1].status = BIT0;
    tmp[numObjects - 1].numPolygons = source->numPolygons;
    tmp[numObjects - 1].numVeritces = source->numVeritces;
    tmp[numObjects - 1].pos[0] = source->pos[0];
    tmp[numObjects - 1].pos[1] = source->pos[1];
    tmp[numObjects - 1].pos[2] = source->pos[2];
    tmp[numObjects - 1].pPolygon = (POLYGON *) malloc(tmp[numObjects - 1].numPolygons * sizeof(POLYGON));
    tmp[numObjects - 1].pVertex = (VERTEX *) malloc(tmp[numObjects - 1].numVeritces * sizeof(VERTEX));
    /* copy polygon list */
    for(i = 0 ; i < tmp[numObjects - 1].numPolygons ; i++)
    {
        tmp[numObjects - 1].pPolygon[i].AdjPoly[0]= source->pPolygon[i].AdjPoly[0];
        tmp[numObjects - 1].pPolygon[i].AdjPoly[1]= source->pPolygon[i].AdjPoly[1];
        tmp[numObjects - 1].pPolygon[i].AdjPoly[2]= source->pPolygon[i].AdjPoly[2];
        tmp[numObjects - 1].pPolygon[i].R = source->pPolygon[i].R;
        tmp[numObjects - 1].pPolygon[i].G = source->pPolygon[i].G;
        tmp[numObjects - 1].pPolygon[i].B = source->pPolygon[i].B;
        tmp[numObjects - 1].pPolygon[i].vertexID[0]= source->pPolygon[i].vertexID[0];
        tmp[numObjects - 1].pPolygon[i].vertexID[1]= source->pPolygon[i].vertexID[1];
        tmp[numObjects - 1].pPolygon[i].vertexID[2]= source->pPolygon[i].vertexID[2];
        tmp[numObjects - 1].pPolygon[i].n[0]= source->pPolygon[i].n[0];
        tmp[numObjects - 1].pPolygon[i].n[1]= source->pPolygon[i].n[1];
        tmp[numObjects - 1].pPolygon[i].n[2]= source->pPolygon[i].n[2];

    }

    /*copy vertex list */
    for(i = 0 ; i < tmp[numObjects - 1].numVeritces ; i++)
    {
        tmp[numObjects - 1].pVertex[i].pos[0] = source->pVertex[i].pos[0];
        tmp[numObjects - 1].pVertex[i].pos[1] = source->pVertex[i].pos[1];
        tmp[numObjects - 1].pVertex[i].pos[2] = source->pVertex[i].pos[2];
        tmp[numObjects - 1].pVertex[i].n[0]= source->pVertex[i].n[0];
        tmp[numObjects - 1].pVertex[i].n[1]= source->pVertex[i].n[1];
        tmp[numObjects - 1].pVertex[i].n[2]= source->pVertex[i].n[2];
        tmp[numObjects - 1].pVertex[i].textureX = source->pVertex[i].textureX;
        tmp[numObjects - 1].pVertex[i].textureY = source->pVertex[i].textureY;
        tmp[numObjects - 1].pVertex[i].numAdjPoly = source->pVertex[i].numAdjPoly;
        tmp[numObjects - 1].pVertex[i].numAdjVert = source->pVertex[i].numAdjVert;
        tmp[numObjects - 1].pVertex[i].pAdjPoly = (unsigned int *)malloc(4 * tmp[numObjects - 1].pVertex[i].numAdjPoly);
        tmp[numObjects - 1].pVertex[i].pAdjVert = (unsigned int *)malloc(4 * tmp[numObjects - 1].pVertex[i].numAdjVert);
        for(j = 0 ; j < tmp[numObjects - 1].pVertex[i].numAdjPoly ; j++)
            tmp[numObjects - 1].pVertex[i].pAdjPoly[j] = source->pVertex[i].pAdjPoly[j];
        for(j = 0 ; j < tmp[numObjects - 1].pVertex[i].numAdjVert ; j++)
            tmp[numObjects - 1].pVertex[i].pAdjVert[j] = source->pVertex[i].pAdjVert[j];
    }
    if(numObjects > 1)
    {
        free(*dest);
    }
    *dest = tmp; 
}

void shadow()
{
    int i, j, k = 0;
    float shadowX, shadowZ;
    if((final.pCurrentObj->status & BIT2) == BIT2)
        return;
    move(t1, -camera.pos[0], -camera.pos[1], -camera.pos[2]);
    rotate(t2, 0, -camera.heading, 0);
    rotate(t3, 0, 0, -camera.pitch);
    rotate(t4, -camera.tilt, 0, 0);
    matrixMul(t4, t3);
    matrixMul(t4, t2);
    matrixMul(t4, t1);
    for(i = 0 ; i < final.pCurrentObj->numPolygons ; i++)
    {
        if(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[1] < 1 || 
           final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[1] > light.pos[1]-40
           || final.pCurrentObj->pPolygon[i].n[1] < -0.8)
        {
            final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[0] = 0xfffffff;
            continue;
        }
        if(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[1] < 1 || 
           final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[1] > light.pos[1]-40)
        {
            final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[0] = 0xfffffff;
            continue;
        }
        if(final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[1] < 1 || 
           final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[1] > light.pos[1]-40)
        {
            final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[0] = 0xfffffff;
            continue;
        }
        
        shadowX = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[0] + 
            ((final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[0] - light.pos[0]))*
            (1 - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[1])/
            ((final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[1] - light.pos[1]));
        shadowZ = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[2] + 
            ((final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[2] - light.pos[2]))*
            (1 - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[1])/
            ((final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[1] - light.pos[1]));
        if(shadowX > 880 || shadowX < -250 || shadowZ > 650 || shadowZ < -470)
        {
            final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[0] = 0xfffffff;
            continue;
        }
        for(j = 0 ; j < 3 ; j++)
        {
            final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[j] = (shadowX * t4->m[j][0]) + t4->m[j][1] +
                     (shadowZ * t4->m[j][2]) + (t4->m[j][3]);
        }
        if(final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[0] < 1)
        {
            final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[0]].pos[0] = 0xfffffff;
            continue;
        } 
        shadowX = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[0] + 
            ((final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[0] - light.pos[0]))*
            (1 - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[1])/
            ((final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[1] - light.pos[1]));
        shadowZ = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[2] + 
            ((final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[2] - light.pos[2]))*
            (1 - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[1])/
            ((final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[1] - light.pos[1]));

        if(shadowX > 880 || shadowX < -250 || shadowZ > 650 || shadowZ < -470)
        {
            final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[0] = 0xfffffff;
            continue;
        }
        for(j = 0 ; j < 3 ; j++)
        {
            final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[j] = (shadowX * t4->m[j][0]) + t4->m[j][1] +
                     (shadowZ * t4->m[j][2]) + (t4->m[j][3]);
        }
        if(final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[0] < 1)
        {
            final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[1]].pos[0] = 0xfffffff;
            continue;
        }               
        shadowX = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[0] + 
            ((final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[0] - light.pos[0]))*
            (1 - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[1])/
            ((final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[1] - light.pos[1]));
        shadowZ = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[2] + 
            ((final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[2] - light.pos[2]))*
            (1 - final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[1])/
            ((final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[1] - light.pos[1]));

        if(shadowX > 880 || shadowX < -250 || shadowZ > 650 || shadowZ < -470)
        {
            final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[0] = 0xfffffff;
            continue;
        }
        for(j = 0 ; j < 3 ; j++)
        {
            final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[j] = (shadowX * t4->m[j][0]) + t4->m[j][1] +
                     (shadowZ * t4->m[j][2]) + (t4->m[j][3]);
        }
        if(final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[0] < 1)
        {
            final.vertexList[final.pCurrentObj->pPolygon[i].vertexID[2]].pos[0] = 0xfffffff;
            continue;
        }               
        final.polyIdList[k++] = i;
    }
    final.seenNumPoly = k;
    projection();
    flatShade(1);  
}


void textureMap(BMP * bitmap,int command)
{
    int i,ymin, ymax, ymid, xmin, xmax, xmid, y, x, dy1, dx1, dy2, dx2;
    float zmin, zmid, zmax, z, dz1, dz2;
    float txmin, txmid, txmax, tymin, tymid, tymax,dtx1, dtx2, dty1, dty2,txint1, txint2, tyint1, tyint2,tx,ty,ftmp;
    int xint1, xint2, zint1, zint2,tmp, tmpx1, tmpx2, tmpy1, tmpy2;

    for(i = 0 ; i < final.seenNumPoly ; i++)
    {
        if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] <= final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] &&
           final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] <= final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]])
        {
                ymin = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                xmin = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                zmin = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                txmin = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].textureX/zmin;
                tymin = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].textureY/zmin;
                if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] < final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]])
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                    txmax = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].textureX/zmax;
                    txmid = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].textureX/zmid;
                    tymax = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].textureY/zmax;
                    tymid = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].textureY/zmid;
                }
                else
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                    txmax = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].textureX/zmax;
                    txmid = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].textureX/zmid;
                    tymax = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].textureY/zmax;
                    tymid = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].textureY/zmid;
                }
        }
        else if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] <= final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] &&
           final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] <= final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]])
        {
                ymin = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                xmin = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                zmin = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                txmin = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].textureX/zmin;
                tymin = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].textureY/zmin;

                if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]] < final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]])
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                    txmax = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].textureX/zmax;
                    txmid = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].textureX/zmid;
                    tymax = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].textureY/zmax;
                    tymid = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].textureY/zmid;
                }
                else
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                    txmax = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].textureX/zmax;
                    txmid = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].textureX/zmid;
                    tymax = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].textureY/zmax;
                    tymid = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].textureY/zmid;
                }
        }
        else
        {
                ymin = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                xmin = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]];
                zmin = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].pos[0];
                txmin = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].textureX/zmin;
                tymin = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[2]].textureY/zmin;

                if(final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]] < final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]])
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                    txmax = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].textureX/zmax;
                    txmid = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].textureX/zmid;
                    tymax = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].textureY/zmax;
                    tymid = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].textureY/zmid;
                }
                else
                {
                    ymax = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    ymid = final.projY[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    xmax = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]];
                    xmid = final.projX[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]];
                    zmax = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].pos[0];
                    zmid = final.vertexList[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].pos[0];
                    txmax = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].textureX/zmax;
                    txmid = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].textureX/zmid;
                    tymax = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[1]].textureY/zmax;
                    tymid = final.pCurrentObj->pVertex[final.pCurrentObj->pPolygon[final.polyIdList[i]].vertexID[0]].textureY/zmid;
                }
        }
        if(ymax < 0 || ymin > VERTICAL || (xmin < 0 && xmid < 0 && xmax < 0) ||
           (xmin > HORIZONTAL && xmid > HORIZONTAL && xmax > HORIZONTAL))
           continue;
        dy1 = ymid - ymin;
        dx1 = xmid - xmin;
        dz1 = zmid - zmin;
        dtx1 = txmid - txmin;
        dty1 = tymid - tymin;
        dy2 = ymax - ymin;
        dx2 = xmax - xmin;
        dz2 = zmax - zmin;
        dtx2 = txmax - txmin;
        dty2 = tymax - tymin;
        if(ymin < 0)
            tmpy1 = 0;
        else if( ymin > VERTICAL)
            tmpy1 = VERTICAL - 2;
        else
            tmpy1 = ymin;
        if(ymid < 0)
            tmpy2 = 0;
        else if( ymid > VERTICAL)
            tmpy2 = VERTICAL - 2;
        else
            tmpy2 = ymid;
        for(y = tmpy1 ; y < tmpy2 ; y++)
        {
            xint1 = xmin + (y - ymin)*dx1/dy1;
            xint2 = xmin + (y - ymin)*dx2/dy2;
            zint1 = zmin + (y - ymin)*dz1/dy1;
            zint2 = zmin + (y - ymin)*dz2/dy2;
            txint1 = txmin + (float)(y - ymin) * dtx1/(float)dy1;
            txint2 = txmin + (float)(y - ymin) * dtx2/(float)dy2;
            tyint1 = tymin + (float)(y - ymin) * dty1/(float)dy1;
            tyint2 = tymin + (float)(y - ymin) * dty2/(float)dy2;
            if(xint1 > xint2)
            {
                tmp = xint1;
                xint1 = xint2;
                xint2 = tmp;
                tmp = zint1;
                zint1 = zint2;
                zint2 = tmp;
                ftmp = txint1;
                txint1 = txint2;
                txint2 = ftmp;
                ftmp = tyint1;
                tyint1 = tyint2;
                tyint2 = ftmp;
            }
            if(xint1 < 0)
                tmpx1 = 0;
            else if( xint1 > HORIZONTAL)
                tmpx1 = HORIZONTAL - 2;
            else
                tmpx1 = xint1;                                                    
            if(xint2 < 0)
                tmpx2 = 0;
            else if( xint2 > HORIZONTAL)
                tmpx2 = HORIZONTAL - 2;
            else
                tmpx2 = xint2;
            for(x = tmpx1 ; x <= tmpx2 ; x++)
            {
                tx = txint1 + (float)(x - xint1) * ( txint2 - txint1)/(float)(xint2 - xint1);
                ty = tyint1 + (float)(x - xint1) * ( tyint2 - tyint1)/(float)(xint2 - xint1);
                z = zint1 + (x - xint1) * (zint2 - zint1)/(xint2 - xint1);
                if(GT_READWORD(zBuffer + x + (y * HORIZONTAL)) >= z)
                {
                    switch(command)
                    {
                    case 0:
                        GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/),
                                      GT_READCHAR((int)(bitmap->data) +(int)( z*tx *(bitmap->bitMapInfoHeaderPointer->width)*3 - (int)(z*tx *(bitmap->bitMapInfoHeaderPointer->width)*3)%3)
                                                   + ((int)(z*ty * (bitmap->bitMapInfoHeaderPointer->height)) *(bitmap->bitMapInfoHeaderPointer->width)*3)));
                        GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/+1),
                                      GT_READCHAR((int)(bitmap->data) +(int)( z*tx *(bitmap->bitMapInfoHeaderPointer->width)*3+1 - (int)(z*tx *(bitmap->bitMapInfoHeaderPointer->width)*3)%3)
                                                   + ((int)(z*ty * (bitmap->bitMapInfoHeaderPointer->height)) *(bitmap->bitMapInfoHeaderPointer->width)*3)));
                        GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/+2),
                                      GT_READCHAR((int)(bitmap->data) +(int)( z*tx *(bitmap->bitMapInfoHeaderPointer->width)*3+2 - (int)(z*tx *(bitmap->bitMapInfoHeaderPointer->width)*3)%3)
                                                   + ((int)(z*ty * (bitmap->bitMapInfoHeaderPointer->height)) *(bitmap->bitMapInfoHeaderPointer->width)*3)));
                        break;
                    case 1:
                        GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/), (tx+ty)*255*z);
                        GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/+1),(2-tx+ty)*255*z );
                        GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/+2), (tx+ty)*255*z);
                        break;
                    }
                    GT_WRITE_WORD(zBuffer + x + (y * HORIZONTAL) , z);
                }
            }
        }
        dy1 = ymax - ymid;
        dx1 = xmax - xmid;
        dz1 = zmax - zmid;
        dtx1 = txmax - txmid;
        dty1 = tymax - tymid;
        if(ymax < 0)
            tmpy1 = 0;
        else if( ymax > VERTICAL)
            tmpy1 = VERTICAL - 2;
        else
            tmpy1 = ymax;
        for(y = tmpy2 ; y < tmpy1 ; y++)
        {
            xint1 = xmid + (y - ymid)*dx1/dy1;
            xint2 = xmin + (y - ymin)*dx2/dy2;
            zint1 = zmid + (y - ymid)*dz1/dy1;
            zint2 = zmin + (y - ymin)*dz2/dy2;
            txint1 = txmid + (float)(y - ymid) * dtx1/(float)dy1;
            txint2 = txmin + (float)(y - ymin) * dtx2/(float)dy2;
            tyint1 = tymid + (float)(y - ymid) * dty1/(float)dy1;
            tyint2 = tymin + (float)(y - ymin) * dty2/(float)dy2;
            if(xint1 > xint2)
            {
                tmp = xint1;
                xint1 = xint2;
                xint2 = tmp;
                tmp = zint1;
                zint1 = zint2;
                zint2 = tmp;
                ftmp = txint1;
                txint1 = txint2;
                txint2 = ftmp;
                ftmp = tyint1;
                tyint1 = tyint2;
                tyint2 = ftmp;
            }
            if(xint1 < 0)
                tmpx1 = 0;
            else if( xint1 > HORIZONTAL)
                tmpx1 = HORIZONTAL - 2;
            else
                tmpx1 = xint1;
            if(xint2 < 0)
                tmpx2 = 0;
            else if( xint2 > HORIZONTAL)
                tmpx2 = HORIZONTAL - 2;
            else
                tmpx2 = xint2;
            for(x = tmpx1 ; x <= tmpx2 ; x++)
            {
                tx = txint1 + (float)(x - xint1) * ( txint2 - txint1)/(float)(xint2 - xint1);
                ty = tyint1 + (float)(x - xint1) * ( tyint2 - tyint1)/(float)(xint2 - xint1);
                z = zint1 + (x - xint1) * (zint2 - zint1)/(xint2 - xint1);
                if(GT_READWORD(zBuffer + x + (y * HORIZONTAL)) >= z)
                {
                    switch(command)
                    {
                    case 0:
                        GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/),
                                      GT_READCHAR((int)(bitmap->data) +(int)( z*tx *(bitmap->bitMapInfoHeaderPointer->width)*3 - (int)(z*tx *(bitmap->bitMapInfoHeaderPointer->width)*3)%3)
                                                   + ((int)(z*ty * (bitmap->bitMapInfoHeaderPointer->height)) *(bitmap->bitMapInfoHeaderPointer->width)*3)));
                        GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/+1),
                                      GT_READCHAR((int)(bitmap->data) +(int)( z*tx *(bitmap->bitMapInfoHeaderPointer->width)*3+1 - (int)(z*tx *(bitmap->bitMapInfoHeaderPointer->width)*3)%3)
                                                   + ((int)(z*ty * (bitmap->bitMapInfoHeaderPointer->height)) *(bitmap->bitMapInfoHeaderPointer->width)*3)));
                        GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/+2),
                                      GT_READCHAR((int)(bitmap->data) +(int)( z*tx *(bitmap->bitMapInfoHeaderPointer->width)*3+2 - (int)(z*tx *(bitmap->bitMapInfoHeaderPointer->width)*3)%3)
                                                   + ((int)(z*ty * (bitmap->bitMapInfoHeaderPointer->height)) *(bitmap->bitMapInfoHeaderPointer->width)*3)));
                        break;
                    case 1:
                        GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/), (tx+ty)*255*z);
                        GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/+1),(2-tx+ty)*255*z );
                        GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/+2), (tx+ty)*255*z);
                        break;
                    }
                    GT_WRITE_WORD(zBuffer + x + (y * HORIZONTAL) , z);
                }
            }
        }
    } 
}

void fog(float density)
{
    int x,y,z;
    float f,rnd;
    for(y = 0 ; y <= VERTICAL ; y++)
        for(x = 0 ; x <= HORIZONTAL ; x++)
        {
            if(x % 10 == 0)
                rnd = ((float)rand()/33000)*10-5;
            z = GT_READWORD(zBuffer + x + (y * HORIZONTAL));
            if(z < 0xfffffff)
            {
                f = pow(2.71,-((float)z)*density);
                GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/),
                              GT_READCHAR(frameBuffer + x*3 + (y << 11)/*(y*2048)*/) * f + 160 *(1-f)+rnd);
                GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/+1),
                              GT_READCHAR(frameBuffer + x*3 + (y << 11)/*(y*2048)*/+1) * f + 160 *(1-f)+rnd);
                GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/+2),
                              GT_READCHAR(frameBuffer + x*3 + (y << 11)/*(y*2048)*/+2) * f + 160 *(1-f)+rnd);
            }
            else
            {
               GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/),
                              160+rnd);
                GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/+1),
                              160+rnd );
                GT_WRITE_CHAR((frameBuffer + x*3 + (y << 11)/*(y*2048)*/+2),
                              160+rnd);
            }

        }

}

void lightDisplay()
{
    int x,y,rad,a,xmid,ymid,r,g,b,tempAddress;
    float t,camx,camy,camz,sx,sy;


    sx = HORIZONTAL / 42.44 *35 ;  /* sx=Xmax/2 * Fr/21.22*Ax*/
    sy = VERTICAL / 42.44 * 35;
    move(t1, -camera.pos[0], -camera.pos[1], -camera.pos[2]);
    rotate(t2, 0, -camera.heading, 0);
    rotate(t3, 0, 0, -camera.pitch);
    rotate(t4, -camera.tilt, 0, 0);
    matrixMul(t4, t3);
    matrixMul(t4, t2);
    matrixMul(t4, t1);

    camx = (light.pos[0] * t4->m[0][0]) + (light.pos[1] * t4->m[0][1]) +
             (light.pos[2] * t4->m[0][2]) + (t4->m[0][3]);
    camy = (light.pos[0] * t4->m[1][0]) + (light.pos[1] * t4->m[1][1]) +
             (light.pos[2] * t4->m[1][2]) + (t4->m[1][3]);
    camz = (light.pos[0] * t4->m[2][0]) + (light.pos[1] * t4->m[2][1]) +
             (light.pos[2] * t4->m[2][2]) + (t4->m[2][3]);

    if (camx <=0 )
        return;

    xmid = HORIZONTAL / 2 + camz / camx * sx;
    ymid = VERTICAL / 2 - camy / camx * sy;

    t= 4000/sqrt(pow(camx,2)+pow(camy,2)+pow(camz,2));

    for(rad = 0 ; rad <= t ; rad++)
    {
        if((light.B-3*(rad)) >= 0)
        {
            b = light.B-3*(rad);
        }
        else
            b = 0;
        if((light.G-3*(rad)) >= 0)
        {
            g = light.G-3*(rad);
        }
        else
            g = 0;
        if((light.R-3*(rad)) >= 0)
        {
            r = light.R-3*(rad);
        }
        else
            r = 0;
        for(a = 0 ; a < 360 ; a++)
        {
            x = xmid + rad * gcos(a);
            y = ymid + rad * gsin(a);
            if(GT_READWORD(zBuffer + x + (y * HORIZONTAL)) >= camz)
            {
                tempAddress = frameBuffer + x*3 + (y << 11)/*(y*2048)*/;
                GT_WRITE_CHAR(tempAddress    ,b);
                GT_WRITE_CHAR(tempAddress + 1,g);
                GT_WRITE_CHAR(tempAddress + 2,r);
            }
        }
    }
        for(rad = t+7 ; rad <= t+8 ; rad++)
            for(a = 0 ; a < 360 ; a++)
            {
                x = xmid + rad * gcos(a);
                y = ymid + rad * gsin(a);
                if(GT_READWORD(zBuffer + x + (y * HORIZONTAL)) >= camz)
                {
                    tempAddress = frameBuffer + x*3 + (y << 11)/*(y*2048)*/;
                    GT_WRITE_CHAR(tempAddress    ,light.B );
                    GT_WRITE_CHAR(tempAddress + 1,light.G );
                    GT_WRITE_CHAR(tempAddress + 2,light.R);
                }
            }
}

void colorObject(OBJECT * pObj, int R, int G, int B)
{
    int i;
    for(i = 0 ; i < pObj->numPolygons ; i++)
    {
        pObj->pPolygon[i].R = R;
        pObj->pPolygon[i].G = G;
        pObj->pPolygon[i].B = B;
    }
}
