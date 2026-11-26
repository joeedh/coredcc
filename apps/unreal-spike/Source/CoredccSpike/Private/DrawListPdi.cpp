#include "DrawListPdi.h"

#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "DynamicMeshBuilder.h"
#include "PrimitiveDrawInterface.h"
#include "SceneManagement.h"

#include <array>
#include <cmath>
#include <cstring>
#include <vector>

using coredcc::DrawRecord;
using coredcc::DrawRecordKind;
using coredcc::DrawListReader;

namespace
{
using M4 = std::array<double, 16>;

M4 Mul(const M4& A, const M4& B)
{
	M4 Out{};
	for (int C = 0; C < 4; C++)
		for (int R = 0; R < 4; R++)
		{
			double S = 0;
			for (int K = 0; K < 4; K++) S += A[K * 4 + R] * B[C * 4 + K];
			Out[C * 4 + R] = S;
		}
	return Out;
}

M4 Translation(double X, double Y, double Z)
{
	return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, X, Y, Z, 1};
}

M4 FromMat(const coredcc::mat4d& M)
{
	M4 Out;
	const double* D = M;
	for (int I = 0; I < 16; I++) Out[I] = D[I];
	return Out;
}

FVector Apply(const M4& M, double X, double Y, double Z)
{
	return FVector(M[0] * X + M[4] * Y + M[8] * Z + M[12], M[1] * X + M[5] * Y + M[9] * Z + M[13],
		M[2] * X + M[6] * Y + M[10] * Z + M[14]);
}

FLinearColor ColorOf(uint32 C)
{
	return FLinearColor(FColor(C & 255, (C >> 8) & 255, (C >> 16) & 255, (C >> 24) & 255));
}

FColor RawColor(uint32 C)
{
	return FColor(C & 255, (C >> 8) & 255, (C >> 16) & 255, (C >> 24) & 255);
}

uint8 GroupOf(coredcc::DepthMode Depth)
{
	return Depth == coredcc::DepthMode::Test ? SDPG_World : SDPG_Foreground;
}

/* The format's shapes (docs/drawlist-format.md), as triangles in their own space. */
void Lathe(std::vector<FVector>& Out, std::initializer_list<std::pair<double, double>> Profile)
{
	const std::vector<std::pair<double, double>> P(Profile);
	const int N = 24;
	for (int S = 0; S < N; S++)
	{
		const double A0 = 2 * PI * S / N, A1 = 2 * PI * (S + 1) / N;
		for (size_t K = 0; K + 1 < P.size(); K++)
		{
			const auto [R0, Z0] = P[K];
			const auto [R1, Z1] = P[K + 1];
			const FVector Q[4] = {{R0 * cos(A0), R0 * sin(A0), Z0}, {R0 * cos(A1), R0 * sin(A1), Z0},
				{R1 * cos(A1), R1 * sin(A1), Z1}, {R1 * cos(A0), R1 * sin(A0), Z1}};
			Out.insert(Out.end(), {Q[0], Q[1], Q[2], Q[0], Q[2], Q[3]});
		}
	}
}

std::vector<FVector> ShapeMesh(coredcc::ShapeKind Kind)
{
	std::vector<FVector> Out;
	switch (Kind)
	{
	case coredcc::ShapeKind::Arrow:
		Lathe(Out, {{0, 0}, {0.02, 0}, {0.02, 0.8}, {0.07, 0.8}, {0, 1}});
		break;
	case coredcc::ShapeKind::Cone:
		Lathe(Out, {{0, 0}, {0.5, 0}, {0, 1}});
		break;
	case coredcc::ShapeKind::Disc:
		Lathe(Out, {{0, 0}, {1, 0}});
		break;
	case coredcc::ShapeKind::Sphere:
		Lathe(Out, {{0, -0.5}, {0.35, -0.35}, {0.5, 0}, {0.35, 0.35}, {0, 0.5}});
		break;
	case coredcc::ShapeKind::Ring:
		/* A flat band of the torus's width: enough to see and count. */
		Lathe(Out, {{0.98, 0}, {1.02, 0}});
		break;
	case coredcc::ShapeKind::Quad:
		Out = {{-0.5, -0.5, 0}, {0.5, -0.5, 0}, {0.5, 0.5, 0}, {-0.5, -0.5, 0}, {0.5, 0.5, 0}, {-0.5, 0.5, 0}};
		break;
	case coredcc::ShapeKind::Cube:
		for (int Axis = 0; Axis < 3; Axis++)
			for (int Sign = -1; Sign <= 1; Sign += 2)
			{
				auto P = [&](double U, double V) {
					double Q[3];
					Q[Axis] = 0.5 * Sign;
					Q[(Axis + 1) % 3] = 0.5 * U;
					Q[(Axis + 2) % 3] = 0.5 * V;
					return FVector(Q[0], Q[1], Q[2]);
				};
				Out.insert(Out.end(), {P(-1, -1), P(1, -1), P(1, 1), P(-1, -1), P(1, 1), P(-1, 1)});
			}
		break;
	}
	return Out;
}

/* Triangles in the world, drawn about `Origin` so the vertices stay small as floats. */
void DrawMesh(FPrimitiveDrawInterface* PDI, const FMaterialRenderProxy* Material, ERHIFeatureLevel::Type Level,
	const FVector& Origin, const std::vector<FVector>& Corners, const std::vector<FColor>& Colors, uint8 Group)
{
	FDynamicMeshBuilder Builder(Level);
	for (size_t I = 0; I < Corners.size(); I++)
	{
		Builder.AddVertex(FDynamicMeshVertex(FVector3f(Corners[I] - Origin), FVector2f(0, 0), Colors[I]));
	}
	for (int32 I = 0; I + 2 < int32(Corners.size()); I += 3) Builder.AddTriangle(I, I + 1, I + 2);
	Builder.Draw(PDI, FTranslationMatrix(Origin), Material, Group, true);
}
} // namespace

FDrawListStats DrawDrawListPdi(const coredcc::DrawList& List, FPrimitiveDrawInterface* PDI,
	const FMaterialRenderProxy* Material, ERHIFeatureLevel::Type FeatureLevel)
{
	FDrawListStats Stats;
	DrawListReader Reader;
	if (!Reader.read(List.bytes()).ok()) return Stats;
	const coredcc::double3& O = Reader.origin();
	const FVector Origin(O[0], O[1], O[2]);
	std::vector<M4> Stack{Translation(O[0], O[1], O[2])};
	for (const DrawRecord& It : Reader.items())
	{
		const M4& Cur = Stack.back();
		const uint8 Group = GroupOf(It.style.depthMode);
		const FLinearColor Color = ColorOf(It.style.color);
		auto V = [&](uint32 I) { return Apply(Cur, It.vertex(I, 0), It.vertex(I, 1), It.vertex(I, 2)); };
		switch (It.kind)
		{
		case DrawRecordKind::PushMatrix:
			Stack.push_back(Mul(Cur, FromMat(It.matrix())));
			break;
		case DrawRecordKind::PopMatrix:
			if (Stack.size() > 1) Stack.pop_back();
			break;
		case DrawRecordKind::Lines:
			for (uint32 I = 0; I + 1 < It.aux; I += 2)
			{
				PDI->DrawTranslucentLine(V(I), V(I + 1), Color, Group, It.style.width);
				Stats.Lines++;
			}
			break;
		case DrawRecordKind::Points:
			for (uint32 I = 0; I < It.aux; I++)
			{
				PDI->DrawPoint(V(I), Color, It.style.width, Group);
				Stats.Points++;
			}
			break;
		case DrawRecordKind::Triangles:
		{
			std::vector<FVector> Corners;
			std::vector<FColor> Colors;
			for (uint32 I = 0; I < It.aux; I++)
			{
				Corners.push_back(V(I));
				Colors.push_back(RawColor(It.flags & 1 ? It.cornerColor(I) : It.style.color));
			}
			if (Material) DrawMesh(PDI, Material, FeatureLevel, Origin, Corners, Colors, Group);
			Stats.Meshes++;
			break;
		}
		case DrawRecordKind::Shape:
		{
			const M4 M = Mul(Cur, FromMat(It.matrix()));
			std::vector<FVector> Corners;
			for (const FVector& P : ShapeMesh(coredcc::ShapeKind(It.aux))) Corners.push_back(Apply(M, P.X, P.Y, P.Z));
			if (Material)
			{
				DrawMesh(PDI, Material, FeatureLevel, Origin, Corners,
					std::vector<FColor>(Corners.size(), RawColor(It.style.color)), Group);
			}
			Stats.Meshes++;
			break;
		}
		case DrawRecordKind::Circle2D:
		case DrawRecordKind::Line2D:
			Stats.Skipped2D++;
			break;
		}
	}
	return Stats;
}

int32 DrawDrawListHud(const coredcc::DrawList& List, FCanvas* Canvas)
{
	DrawListReader Reader;
	if (!Canvas || !Reader.read(List.bytes()).ok()) return 0;
	int32 Drawn = 0;
	for (const DrawRecord& It : Reader.items())
	{
		const FLinearColor Color = ColorOf(It.style.color);
		auto Line = [&](double X0, double Y0, double X1, double Y1) {
			FCanvasLineItem L(FVector2D(X0, Y0), FVector2D(X1, Y1));
			L.SetColor(Color);
			L.LineThickness = It.style.width;
			Canvas->DrawItem(L);
		};
		if (It.kind == DrawRecordKind::Line2D)
		{
			for (uint32 I = 0; I + 1 < It.aux; I++) Line(It.vertex(I, 0), It.vertex(I, 1), It.vertex(I + 1, 0), It.vertex(I + 1, 1));
			Drawn++;
		}
		else if (It.kind == DrawRecordKind::Circle2D)
		{
			float C[3];
			std::memcpy(C, It.payload, sizeof(C)); /* x, y, radius */
			const double X = C[0], Y = C[1], R = C[2];
			for (int S = 0; S < 32; S++)
			{
				const double A0 = 2 * PI * S / 32, A1 = 2 * PI * (S + 1) / 32;
				Line(X + R * cos(A0), Y + R * sin(A0), X + R * cos(A1), Y + R * sin(A1));
			}
			Drawn++;
		}
	}
	return Drawn;
}
