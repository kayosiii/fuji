#if !defined(_MODEL_H)
#define _MODEL_H

#include "Matrix.h"

class Animation;
class Material;

enum CustomDataType
{
	CUST_VertexBuffer,
	CUST_Cloth
};

enum VertexFormat
{
	VF_Position = 1,
	VF_Normal = 2,
	VF_Colour = 4,
	VF_Tex0 = 8,
	VF_Tex1 = 16,
	VF_Illum = 32
};

class ModelData
{
public:
	uint32 IDtag;
	char *pName;

	uint16 subobjectCount;
	uint16 materialCount;
	uint16 boneCount;
	uint16 customDataCount;
	uint32 flags;

	struct MaterialData
	{
		char *pName;
		char *pMaterialDescription;
		Material *pMaterial;
		uint32 reserved;
	} *pMaterials;

	uint32 vertexCount;
	char *pVertexData;
	uint32 indexCount;
	char *pIndexData;

	struct Subobject
	{
		uint16 vertexFormat;
		uint16 vertexSize;
		uint32 vertexCount;
		uint32 vertexOffset;

		uint32 indexCount;
		uint32 indexOffset;

		uint32 materialIndex;

		uint32 reserved[2];
	} *pSubobjects;

	struct CustomData
	{
		uint16 customDataType;
		uint16 ustomDataCount;
		void *pData;
		uint32 res;
		uint32 res2;
	} *pCustomData;

	void FixUpPointers();
	void CollapsePointers();
};

class Model
{
public:
	static Model* Create(char *pFilename);

	void Draw();

	Matrix worldMatrix;

	ModelData *pModelData;
	Animation *pAnimation;
	Matrix *pAnimMatrices;

#if defined(_WINDOWS)
	IDirect3DVertexBuffer9 *pVertexBuffer;
	IDirect3DIndexBuffer9 *pIndexBuffer;
#endif
};

#endif