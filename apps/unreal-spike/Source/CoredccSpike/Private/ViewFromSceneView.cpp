#include "ViewFromSceneView.h"

#include "SceneView.h"

coredcc::Status ViewFromSceneView(const FSceneView& SceneView, coredcc::View& Out)
{
	const FIntRect Rect = SceneView.UnscaledViewRect;
	Out = coredcc::View();
	Out.viewport[0] = Rect.Min.X;
	Out.viewport[1] = Rect.Min.Y;
	Out.viewport[2] = Rect.Width();
	Out.viewport[3] = Rect.Height();
	const FMatrix& V = SceneView.ViewMatrices.GetViewMatrix();
	/* Not GetProjectionNoAAMatrix: the renderer fills that only when it adds jitter, and it is
	 * the identity before. */
	const FMatrix P = SceneView.ViewMatrices.ComputeProjectionNoAAMatrix();
	double* Dv = Out.view;
	double* Dp = Out.proj;
	for (int R = 0; R < 4; R++)
	{
		for (int C = 0; C < 4; C++)
		{
			Dv[R * 4 + C] = V.M[R][C];
			Dp[R * 4 + C] = P.M[R][C];
		}
	}
	Out.ortho = !SceneView.ViewMatrices.IsPerspectiveProjection();
	Out.reversedZ = true;
	/* UE's perspective projections have their far plane at infinity unless one is set. */
	Out.infiniteFar = !Out.ortho && P.M[2][2] == 0.0;
	Out.handedness = coredcc::Handedness::Left;
	Out.up = coredcc::UpAxis::Z;
	Out.dpiScale = 1.0;
	return Out.validate();
}
