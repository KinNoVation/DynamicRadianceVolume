#version 450 core

#include "globalubos.glsl"

// input
layout(location = 0) in vec2 Texcoord;

layout(binding = 0) uniform sampler3D VoxelScene;

// output
layout(location = 0, index = 0) out vec4 FragColor;


bool IntersectBox(vec3 rayOrigin, vec3 rayDir, vec3 aabbMin, vec3 aabbMax, out float firstHit)
{
	vec3 invRayDir = vec3(1.0) / rayDir;
	vec3 tbot = invRayDir * (aabbMin - rayOrigin);
	vec3 ttop = invRayDir * (aabbMax - rayOrigin);
	vec3 tmin = min(ttop, tbot);
	vec3 tmax = max(ttop, tbot);
	vec2 t = max(tmin.xx, tmin.yz);
	firstHit = max(0.0f, max(t.x, t.y));
	t = min(tmax.xx, tmax.yz);
	float lastHit = min(t.x, t.y);
	return firstHit <= lastHit;
}

void main()
{
	const int lod = 0;
	float voxelSizeInWorldLod = VoxelSizeInWorld * pow(2, lod);

	vec4 rayDir4D = vec4(Texcoord * 2.0 - vec2(1.0), 1.0f, 1.0f) * InverseViewProjection;
	vec3 rayDirection = normalize(rayDir4D.xyz / rayDir4D.w - CameraPosition);


	FragColor= vec4(0);	

	float rayHit = 0.0;
	if(IntersectBox(CameraPosition, rayDirection, VolumeWorldMin, VolumeWorldMax, rayHit))
	{
		float stepSize = voxelSizeInWorldLod * 0.1;

		vec3 voxelHitPos = (CameraPosition + (rayHit + stepSize) * rayDirection - VolumeWorldMin) / voxelSizeInWorldLod;
		float totalIntensity = 0.0f;

		vec3 rayMarchStep = rayDirection * stepSize / voxelSizeInWorldLod;
		ivec3 lastFetch = ivec3(-1);

		// Simple raycast - an actual stepping through the volume would be much better of course.
		vec3 voxelVolumeSize = vec3(textureSize(VoxelScene, lod));
		while(all(greaterThanEqual(voxelHitPos, vec3(0))) && 
			  all(lessThan(voxelHitPos, voxelVolumeSize)))
		{
			// Check current voxel.
			ivec3 voxelCoord = ivec3(voxelHitPos + vec3(0.5));
			if(voxelCoord != lastFetch)
			{
				lastFetch = voxelCoord;
				float voxelIntensity = texelFetch(VoxelScene, voxelCoord, lod).r;	
				totalIntensity += voxelIntensity;
				
				FragColor = vec4(mod(voxelCoord/256.0, vec3(1.0)), 1.0) * voxelIntensity;
				vec3 midDist = abs(vec3(voxelCoord) - voxelHitPos);//  - vec3(0.5);
				//float d = pow(abs(midDist.x) + abs(midDist.y) + abs(midDist.z), 10);
				//FragColor.xyz += vec3(d) * 0.05 * voxelIntensity; //vec3(pow(min(min(midDist.x, midDist.y),midDist.z), 1.0));
				
				// Quick'n dirty!
				float midmid = midDist.x;
				if(midDist.y < midDist.z && midDist.y > midDist.x)
					midmid = midDist.y;
				else if(midDist.y < midDist.x && midDist.y > midDist.z)
					midmid = midDist.y;
				else if(midDist.z < midDist.y && midDist.z > midDist.x)
					midmid = midDist.z;
				else if(midDist.z < midDist.x && midDist.z > midDist.y)
					midmid = midDist.z;

				FragColor.xyz += vec3(pow(midmid, 10) * 500);
				

				if(totalIntensity >= 0.98)
				{
					break;
				}
			}

			voxelHitPos += rayMarchStep * stepSize;
			rayMarchStep *= 1.01;
		}

		//FragColor = vec4(mix(FragColor.xyz, vec3(1.0), totalIntensity), 0);
	}
	else
		FragColor = vec4(abs(rayDirection), 1.0);
}
