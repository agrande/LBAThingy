#pragma once

#include "CoreMinimal.h"

/**
 * LBA2 world conventions.
 *
 * LBA space is right-handed, Y up, angles in 1/4096 of a turn, and a yaw (beta) of 0 faces +Z.
 * Unreal is left-handed, Z up. We map LBA (x, y, z) to Unreal (x, z, y): a single axis swap that turns
 * the right-handed space into the left-handed one without mirroring anything visually.
 * Under that mapping LBA's +Z (character forward) becomes Unreal +Y, which matches how skeletal
 * meshes are conventionally authored for ACharacter (mesh rotated -90 degrees yaw).
 */
namespace LBA
{
	inline constexpr int32 BrickSizeXZ = 512;
	inline constexpr int32 BrickSizeY = 256;
	inline constexpr int32 GridSizeXZ = 64;
	inline constexpr int32 GridSizeY = 25;
	inline constexpr int32 IslandCubeSize = 0x8000;
	inline constexpr int32 AngleFullTurn = 4096;

	/**
	 * Custom depth stencil value of moving objects (characters). The palette shading post-process shades them
	 * smoothly instead of with world-space dither dots, which would slide over them.
	 */
	inline constexpr int32 MovingObjectStencil = 200;

	/** Converts an LBA position (in LBA units) into Unreal centimeters. */
	inline FVector ToUnreal(double X, double Y, double Z, double Scale)
	{
		return FVector(X, Z, Y) * Scale;
	}

	inline FVector ToUnreal(const FVector& LbaPos, double Scale)
	{
		return ToUnreal(LbaPos.X, LbaPos.Y, LbaPos.Z, Scale);
	}

	/** Converts an LBA direction (unit-less) into an Unreal direction. */
	inline FVector DirToUnreal(double X, double Y, double Z)
	{
		return FVector(X, Z, Y);
	}

	inline double AngleToDegrees(double Angle)
	{
		return Angle * 360.0 / AngleFullTurn;
	}

	/** LBA rotations about +Y (right-handed) become negative yaw in Unreal. */
	inline FRotator BetaToRotator(double Beta)
	{
		return FRotator(0.0, -AngleToDegrees(Beta), 0.0);
	}

	/**
	 * Converts an LBA rotation matrix (column vectors, right-handed) to an Unreal quaternion.
	 * The axis swap P is its own inverse, so the converted matrix is P * M * P.
	 */
	inline FQuat MatrixToUnreal(const double M[3][3])
	{
		// Unreal rows/cols: x <- x, y <- z, z <- y
		const int32 Map[3] = { 0, 2, 1 };
		FMatrix Out = FMatrix::Identity;
		for (int32 R = 0; R < 3; ++R)
		{
			for (int32 C = 0; C < 3; ++C)
			{
				// FMatrix uses row vectors (v * M), so transpose while copying.
				Out.M[C][R] = M[Map[R]][Map[C]];
			}
		}
		return FQuat(Out).GetNormalized();
	}

	/**
	 * Builds the LBA bone rotation matrix Rx(alpha) * Rz(gamma) * Ry(beta) used by the original
	 * engine (see RotateMatrix in LIB386) and converts it to an Unreal quaternion.
	 */
	inline FQuat BoneAnglesToUnreal(int32 Alpha, int32 Beta, int32 Gamma)
	{
		const double A = FMath::DegreesToRadians(AngleToDegrees(Alpha));
		const double B = FMath::DegreesToRadians(AngleToDegrees(Beta));
		const double G = FMath::DegreesToRadians(AngleToDegrees(Gamma));
		const double Ca = FMath::Cos(A), Sa = FMath::Sin(A);
		const double Cb = FMath::Cos(B), Sb = FMath::Sin(B);
		const double Cg = FMath::Cos(G), Sg = FMath::Sin(G);

		const double Rx[3][3] = { {1, 0, 0}, {0, Ca, -Sa}, {0, Sa, Ca} };
		const double Rz[3][3] = { {Cg, -Sg, 0}, {Sg, Cg, 0}, {0, 0, 1} };
		const double Ry[3][3] = { {Cb, 0, Sb}, {0, 1, 0}, {-Sb, 0, Cb} };

		auto Mul = [](const double L[3][3], const double R[3][3], double Out[3][3])
		{
			for (int32 I = 0; I < 3; ++I)
			{
				for (int32 J = 0; J < 3; ++J)
				{
					Out[I][J] = L[I][0] * R[0][J] + L[I][1] * R[1][J] + L[I][2] * R[2][J];
				}
			}
		};

		double XZ[3][3];
		double XZY[3][3];
		Mul(Rx, Rz, XZ);
		Mul(XZ, Ry, XZY);
		return MatrixToUnreal(XZY);
	}
}
