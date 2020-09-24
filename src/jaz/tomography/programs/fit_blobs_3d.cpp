#include "fit_blobs_3d.h"
#include <src/jaz/membrane/tilt_space_blob_fit.h>
#include <src/jaz/membrane/blob_fit_3d.h>
#include <src/jaz/membrane/membrane_segmentation.h>
#include <src/jaz/optimization/gradient_descent.h>
#include <src/jaz/optimization/nelder_mead.h>
#include <src/jaz/optimization/lbfgs.h>
#include <src/jaz/optics/damage.h>
#include <src/jaz/tomography/fiducials.h>
#include <src/jaz/tomography/tomogram_set.h>
#include <src/jaz/util/zio.h>
#include <src/jaz/util/log.h>


using namespace gravis;


void FitBlobs3DProgram::readParameters(int argc, char *argv[])
{
	IOParser parser;
	
	double sphere_thickness_0;

	try
	{
		parser.setCommandLine(argc, argv);
		int gen_section = parser.addSection("General options");

		tomoSetFn = parser.getOption("--t", "Tomogram set filename", "tomograms.star");
		listFn = parser.getOption("--i", "File containing a list of tomogram-name/spheres-file pairs");

		sphere_thickness_0 = textToDouble(parser.getOption("--th", "Sphere thickness (same units as sphere centres)"));
		spheres_binning = textToDouble(parser.getOption("--sbin", "Binning factor of the sphere coordinates"));

		fiducials_radius_A = textToDouble(parser.getOption("--frad", "Fiducial marker radius [Å]", "100"));

		prior_sigma_A = textToDouble(parser.getOption("--sig", "Uncertainty std. dev. of initial position [Å]", "10"));
		max_binning = textToDouble(parser.getOption("--bin0", "Initial (maximal) binning factor", "8"));
		min_binning = textToDouble(parser.getOption("--bin1", "Final (minimal) binning factor", "2"));

		diag = parser.checkOption("--diag", "Write out diagnostic information");

		SH_bands = textToInteger(parser.getOption("--n", "Number of spherical harmonics bands", "2"));
		highpass_sigma_real_A = textToDouble(parser.getOption("--hp", "High-pass sigma [Å, real space]", "300"));
		max_iters = textToInteger(parser.getOption("--max_iters", "Maximum number of iterations", "1000"));
		num_threads = textToInteger(parser.getOption("--j", "Number of OMP threads", "6"));

		outPath = parser.getOption("--o", "Output filename pattern");

		Log::readParams(parser);

		if (parser.checkForErrors())
		{
			parser.writeUsage(std::cout);
			exit(1);
		}
	}
	catch (RelionError XE)
	{
		parser.writeUsage(std::cout);
		std::cerr << XE;
		exit(1);
	}

	sphere_thickness = sphere_thickness_0 * spheres_binning;

	outPath = ZIO::makeOutputDir(outPath);

	if (diag)
	{
		ZIO::makeOutputDir(outPath + "diag");
	}
}

void FitBlobs3DProgram::run()
{
	TomogramSet initial_tomogram_set = TomogramSet(tomoSetFn);
	TomogramSet subtracted_tomogram_set = initial_tomogram_set;
	TomogramSet blobs_tomogram_set = initial_tomogram_set;
	
	if (!initial_tomogram_set.globalTable.labelExists(EMDL_TOMO_FIDUCIALS_STARFILE))
	{
		Log::warn("No fiducial markers present: you are advised to run relion_tomo_find_fiducials first.");
	}
	
	std::ifstream list(listFn);
	
	if (!list)
	{
		REPORT_ERROR_STR("Unable to read "+listFn);
	}
	
	std::map<std::string, std::string> tomoToSpheres;
	
	std::string line;

	BufferedImage<float> visualisation(0,0,0);
	
	while (std::getline(list, line))
	{
		std::stringstream sts;
		sts << line;
	
		std::string tomoName, spheresFn;
		sts >> tomoName;
		sts >> spheresFn;
		
		tomoToSpheres[tomoName] = spheresFn;
	}

	int tomo_index = 0;
		
	for (std::map<std::string, std::string>::iterator it = tomoToSpheres.begin();
	     it != tomoToSpheres.end(); it++)
	{
		const std::string tomoName = it->first;
		const std::string spheresFn = it->second;
		
		Log::beginSection("Tomogram " + tomoName);
		
		processTomogram(
			tomoName, spheresFn,
			initial_tomogram_set);
		
		Log::endSection();

		tomo_index++;
	}

	subtracted_tomogram_set.write(outPath + "tomograms.star");
	blobs_tomogram_set.write(outPath + "blob_tomograms.star");

	if (diag)
	{
		visualisation.write(outPath + "diagnostic.mrc");
	}
}

void FitBlobs3DProgram::processTomogram(
		std::string tomoName,
		std::string spheresFn,
		TomogramSet& initial_tomogram_set)
{
	Log::print("Loading tilt series");
	
	spheres = readSpheresCMM(spheresFn, spheres_binning);
	
	const int tomo_index = initial_tomogram_set.getTomogramIndexSafely(tomoName);

	Tomogram tomogram0 = initial_tomogram_set.loadTomogram(tomo_index, true);

	const double pixel_size = tomogram0.optics.pixelSize;

	const double fiducials_radius = fiducials_radius_A / pixel_size;


	std::vector<d3Vector> fiducials(0);
	
	bool has_fiducials = 
	           tomogram0.fiducialsFilename.length() > 0 
	        && tomogram0.fiducialsFilename != "empty";

	        
	Log::print("Filtering");
	
	
	const double segmentation_binning = 2;
	
	Tomogram tomogram_binned = tomogram0.FourierCrop(segmentation_binning, num_threads);
	
	if (has_fiducials)
	{
		Log::print("Erasing fiducial markers");
		
		if (fiducialsDir[fiducialsDir.length()-1] != '/')
		{
			fiducialsDir = fiducialsDir + "/";
		}

		fiducials = Fiducials::read(tomogram0.fiducialsFilename, tomogram0.optics.pixelSize);

		Fiducials::erase(
			fiducials, 
			fiducials_radius / segmentation_binning, 
			tomogram_binned, 
			num_threads);
	}
	
	BufferedImage<float> preweighted_stack = RealSpaceBackprojection::preWeight(
	            tomogram_binned.stack, tomogram_binned.projectionMatrices, num_threads);
	
	Damage::applyWeight(
			preweighted_stack, 
			tomogram_binned.optics.pixelSize, 
			tomogram_binned.cumulativeDose, num_threads);
	
	std::vector<std::vector<double>> all_blob_coeffs;
	Mesh blob_meshes;

	for (int blob_id = 0; blob_id < spheres.size(); blob_id++)
	{
		Log::beginSection("Blob #" + ZIO::itoa(blob_id + 1));
		
		const d4Vector sphere = spheres[blob_id];

		const d3Vector sphere_position = sphere.xyz();
		
		const std::string blob_tag = outPath+tomoName+"_blob_"+ZIO::itoa(blob_id);
		
		std::vector<double> blob_coeffs = segmentBlob(
				sphere_position, 
				sphere.w, 
				sphere_thickness, 
		        segmentation_binning,
				preweighted_stack, 
		        pixel_size,
				tomogram_binned.projectionMatrices,
				diag? blob_tag : "");
		
		all_blob_coeffs.push_back(blob_coeffs);
		
		Mesh blob_mesh = createMesh(blob_coeffs, pixel_size, 50, 20);
		
		blob_mesh.writeObj(blob_tag+".obj");

		MeshBuilder::insert(blob_mesh, blob_meshes);
		Log::endSection();
	}
}

Mesh FitBlobs3DProgram::createMesh(
        const std::vector<double>& blob_coeffs,
        double pixel_size,
        double spacing, 
        double max_tilt_deg)
{
	const double max_tilt = DEG2RAD(max_tilt_deg);
	const double rad = blob_coeffs[3];
	const int azimuth_samples = (int) std::round(2 * PI * rad / spacing);
	const int tilt_samples = (int) std::round(2 * max_tilt * rad / spacing);
	const d3Vector centre(blob_coeffs[0],blob_coeffs[1],blob_coeffs[2]);
	
	const int vertex_count = azimuth_samples * tilt_samples;
	
	Mesh out;
	out.vertices.resize(vertex_count);
	
	const int SH_params = blob_coeffs.size() - 3;
	
	const int SH_bands = (int) sqrt(SH_params - 1);
	SphericalHarmonics SH(SH_bands);
	
	std::vector<double> Y(SH_params);
	
	for (int a = 0; a < azimuth_samples; a++)
	for (int t = 0; t < tilt_samples; t++)
	{
		const double phi   = 2 * PI * a / (double) azimuth_samples;
		const double theta = -max_tilt + 2 * max_tilt * t / (double) (tilt_samples - 1);
		
		SH.computeY(SH_bands, sin(theta), phi, &Y[0]);
		        
		double dist = 0.0;
		
		for (int b = 0; b < SH_params; b++)
		{
			dist += blob_coeffs[b+3] * Y[b];
		}
		
		out.vertices[t * azimuth_samples + a] = 
			pixel_size * (centre + dist * d3Vector(cos(phi), sin(phi), sin(theta)));
	}
	
	const int triangle_count = 2 * (tilt_samples - 1) * azimuth_samples;
	        
	out.triangles.resize(triangle_count);
	
	for (int a = 0; a < azimuth_samples; a++)
	for (int t = 0; t < tilt_samples-1; t++)
	{
		Triangle tri0;
		
		tri0.a =  t      * azimuth_samples +  a;
		tri0.b = (t + 1) * azimuth_samples +  a;
		tri0.c = (t + 1) * azimuth_samples + (a + 1) % azimuth_samples;
		
		Triangle tri1;
		
		tri1.a =  t      * azimuth_samples +  a;
		tri1.b = (t + 1) * azimuth_samples + (a + 1) % azimuth_samples;
		tri1.c =  t      * azimuth_samples + (a + 1) % azimuth_samples;
		
		out.triangles[2 * (t * azimuth_samples + a)    ] = tri0;
		out.triangles[2 * (t * azimuth_samples + a) + 1] = tri1;
	}
	
	return out;
}


std::vector<double> FitBlobs3DProgram::segmentBlob(
        d3Vector sphere_position, 
        double mean_radius_full, 
        double radius_range, 
        double binning, 
        const RawImage<float>& preweighted_stack, 
        double pixel_size,
        const std::vector<d4Matrix>& projections,
        const std::string& debug_prefix)
{
	BufferedImage<float> map = TiltSpaceBlobFit::computeTiltSpaceMap(
			sphere_position, 
			mean_radius_full, 
			radius_range, 
			binning, 
			preweighted_stack, 
			projections);
	
	BufferedImage<d3Vector> directions_XZ = TiltSpaceBlobFit::computeDirectionsXZ(
	            mean_radius_full, binning, projections);
	            
	if (debug_prefix != "")
	{
		map.write(debug_prefix+"_tilt_space_map.mrc");
	}
	
	const int fc = projections.size();
	
	const d4Matrix& A0 = projections[0];
	const d4Matrix& A1 = projections[fc - 1];
	
	const d3Vector tilt_axis_3D = 
			d3Vector(A0(2,0), A0(2,1), A0(2,2)).cross(
			d3Vector(A1(2,0), A1(2,1), A1(2,2))).normalize();
	
	std::vector<double> tilt_axis_azimuth(fc);
	
	for (int f = 0; f < fc; f++)
	{
		const d4Vector t(tilt_axis_3D.x, tilt_axis_3D.y, tilt_axis_3D.z, 0.0);
		const d4Vector tilt_axis_2D = projections[f] * t;
		            
		tilt_axis_azimuth[f] = std::atan2(tilt_axis_2D.y, tilt_axis_2D.x);
	}
	
	const int y_prior = 100 / binning;
	const int falloff = 100 / binning;
	const int width = 10 / binning;
	const double spacing = 40.0 / (pixel_size * binning);
	const double ratio = 5.0;
	const double depth = 0;
	
	const double min_radius_full = mean_radius_full - radius_range/2;
	const double max_radius_full = mean_radius_full + radius_range/2;
		
	
	BufferedImage<float> kernel = MembraneSegmentation::constructMembraneKernel(
		map.xdim, map.ydim, map.zdim, falloff, width, spacing, ratio, depth);   
	
	if (debug_prefix != "")
	{
		kernel.write(debug_prefix+"_tilt_space_kernel.mrc");
	}
	
	BufferedImage<fComplex> map_FS, kernel_FS, correlation_FS;
	
	FFT::FourierTransform(map, map_FS);
	FFT::FourierTransform(kernel, kernel_FS);
	
	correlation_FS.resize(map_FS.xdim, map_FS.ydim, map_FS.zdim);
	
	for (int z = 0; z < map_FS.zdim; z++)
	for (int y = 0; y < map_FS.ydim; y++)
	for (int x = 0; x < map_FS.xdim; x++)
	{
		correlation_FS(x,y,z) = map_FS(x,y,z) * kernel_FS(x,y,z).conj();
	}
	
	BufferedImage<float> correlation;
	        
	FFT::inverseFourierTransform(correlation_FS, correlation);
	
	const double st = map.zdim / 4.0;
	const double s2t = 2 * st * st;
	const double s2f = 2 * y_prior * y_prior;
	        
	for (int f = 0; f < map.zdim; f++)
	for (int y = 0; y < map.ydim; y++)
	for (int x = 0; x < map.xdim; x++)
	{
		const double r = (min_radius_full + radius_range * y / (double) map.ydim) / max_radius_full;
		
		const double ay = map.ydim - y - 1;		        
		const double q0 = 1.0 - exp( -y*y  / s2f);
		const double q1 = 1.0 - exp(-ay*ay / s2f);
		
		const double phi = 2 * PI * x / (double) map.xdim;
		const double mz = (f - map.zdim / 2) * sin(phi - tilt_axis_azimuth[f]);	
		const double qt = exp(-mz*mz / s2t);
		
		correlation(x,y,f) *= r * q0 * q1 * qt;
	}
	
	const float corr_var = Normalization::computeVariance(correlation, 0.f);
	correlation /= sqrt(corr_var);
	
	if (debug_prefix != "")
	{
		correlation.write(debug_prefix+"_tilt_space_correlation.mrc");
	}
	
	const double lambda = 0.00001;
	
	
	TiltSpaceBlobFit blob_pre_fit(0, lambda, correlation, directions_XZ);
	double h0 = blob_pre_fit.estimateInitialHeight();
	
	std::vector<double> last_params = {h0 / blob_pre_fit.basis(0,0,0)};
	
	for (int current_SH_bands = 1; current_SH_bands <= SH_bands; current_SH_bands++)
	{
		TiltSpaceBlobFit blob_fit(current_SH_bands, lambda, correlation, directions_XZ);
		
	    std::vector<double> params(blob_fit.getParameterCount(), 0.0);
		
		for (int i = 0; i < last_params.size() && i < params.size(); i++)
		{
			params[i] = last_params[i];
		}
		
		std::vector<double> final_params = LBFGS::optimize(params, blob_fit, 0, 1000, 1e-6);
		
		
		/*BufferedImage<float> plot = blob_fit.drawSolution(final_params, map);
		plot.write("DEBUG_tilt_space_plot_SH_"+ZIO::itoa(SH_bands)+".mrc");*/
				
		last_params = final_params;
	}
	
	if (debug_prefix != "")
	{
		TiltSpaceBlobFit blob_fit(SH_bands, lambda, correlation, directions_XZ);
		BufferedImage<float> plot = blob_fit.drawSolution(last_params, map);
		
		plot.write(debug_prefix+"_tilt_space_plot_SH_"+ZIO::itoa(SH_bands)+".mrc");
	}
	
	/*BufferedImage<float> surfaceCost(map.xdim, map.ydim, map.zdim);
	BufferedImage<float> indicator(map.xdim, map.ydim, map.zdim);
	
	const float sig_cost = 4;
	        
	for (int f = 0; f < map.zdim; f++)
	for (int y = 0; y < map.ydim; y++)
	for (int x = 0; x < map.xdim; x++)
	{
		float c = correlation(x,y,f);
		if (c < 0) c = 0;
		
		surfaceCost(x,y,f) = exp(-c*c/(sig_cost*sig_cost));
		//indicator(x,y,f) = 1.f - y / (float) (map.ydim - 1);
		//indicator(x,y,f) = 0.5f;
		indicator(x,y,f) = y < map.ydim/2? 1 : 0;
	}
	
	surfaceCost.write("DEBUG_tilt_space_surfaceCost.mrc");
	
	const int iterations = 501;
	
	{
		BufferedImage<f3Vector> xi(map.xdim, map.ydim, map.zdim);
		xi.fill(f3Vector(0,0,0));
		
		BufferedImage<float> u(map.xdim, map.ydim, map.zdim);
		BufferedImage<float> uBar(map.xdim, map.ydim, map.zdim);
		
		u    = indicator;
		uBar = indicator;
				
		const float sigma = 0.5;
		const float tau = 0.5;
		const float nu = 1.8;
		
		
		for (int it = 0; it < iterations; it++)
		{
			for (int f = 0; f < map.zdim; f++)
			for (int y = 0; y < map.ydim; y++)
			for (int x = 0; x < map.xdim; x++)
			{
				const float u0 = uBar(x,y,f);
				const float ux = uBar((x+1)%map.xdim, y, f);
				
				float uy;
				
				if (y == map.ydim - 1)
				{
					uy = uBar(x, y-1, f);
				}
				else
				{
					uy = uBar(x, y+1, f);
				}
				
				float uz;
				
				if (f == fc-1)
				{
					uz = uBar(x, y, f-1);
				}
				else
				{
					uz = uBar(x, y, f+1);
				}
				
				
				const f3Vector grad = f3Vector(ux - u0, uy - u0, uz - u0);
				
				xi(x,y,f) += sigma * grad;
				
				if (xi(x,y,f).norm2() > 1)
				{
					xi(x,y,f) /= xi(x,y,f).length();
				}
			}
			
			for (int f = 0; f < map.zdim; f++)
			for (int y = 0; y < map.ydim; y++)
			for (int x = 0; x < map.xdim; x++)
			{
				const f3Vector g0 = xi(x,y,f);
				const f3Vector gx = xi((x+map.xdim-1)%map.xdim, y, f);
				
				f3Vector gy;
				
				if (y == 0)
				{
					gy = -xi(x, 0, f);
				}
				else
				{
					gy = xi(x, y-1, f);
				}
				
				f3Vector gz;
				
				if (f == 0)
				{
					gz = -xi(x, y, 0);
				}
				else
				{
					gz = xi(x, y, f-1);
				}
				
				const int d = 3;
				const float regional = (y < d? -1 : (y > map.ydim-d-1? 1 : 0));
				
				const float div = (g0.x - gx.x) + (g0.y - gy.y) + (g0.z - gz.z);
				
				const float lastU = u(x,y,f);
				
				const float sc = surfaceCost(x,y,f);
				
				u(x,y,f) += tau * (nu * sc * div - regional);
				
				
				if (u(x,y,f) > 1) 
				{
					u(x,y,f) = 1;
				}
				else if (u(x,y,f) < 0)
				{
					u(x,y,f) = 0;
				}
				
				uBar(x,y,f) = 2 * lastU - u(x,y,f);
			}
			
			if (it % 50 == 0)
			{
				u.write("DEBUG_tilt_space_F_"+ZIO::itoa(it)+"_u.mrc");
				uBar.write("DEBUG_tilt_space_F_"+ZIO::itoa(it)+"_uBar.mrc");
				
				BufferedImage<float> xi_y(map.xdim, map.ydim, map.zdim);
				
				for (int i = 0; i < xi.getSize(); i++)
				{
					xi_y[i] = xi[i].y;
				}
				
				xi_y.write("DEBUG_tilt_space_F_"+ZIO::itoa(it)+"_xi_y.mrc");
			}
		}
	}*/
	
	std::vector<double> out(last_params.size() + 3);
	
	for (int i = 3; i < out.size(); i++)
	{
		out[i] = binning * last_params[i-3];
	}
	
	out[3] += min_radius_full / blob_pre_fit.basis(0,0,0);
	
	SphericalHarmonics SH_3(1);
	std::vector<double> Y_3(4);
	SH_3.computeY(1, 0, 0, &Y_3[0]);
	
	for (int i = 0; i < 3; i++)
	{
		out[i] = sphere_position[i] - out[i+4] * Y_3[0];
		out[i+4] = 0.0;
	}
	
	if (debug_prefix != "")
	{
		BufferedImage<float> plot = TiltSpaceBlobFit::visualiseBlob(
			out, 
			mean_radius_full, 
			radius_range, 
			binning, 
			preweighted_stack, 
			projections);
		
		plot.write(debug_prefix+"_tilt_space_plot_SH_"+ZIO::itoa(SH_bands)+"_blob_space.mrc");
	}
	
	return out;
}

std::vector<d4Vector> FitBlobs3DProgram::readSpheresCMM(
		const std::string& filename,
		double binning)
{
	std::string nextPointKey = "<marker ";

	std::string formatStr =
		"<marker id=\"%d\" x=\"%lf\" y=\"%lf\" z=\"%lf\" r=\"%*f\" g=\"%*f\" b=\"%*f\" radius=\"%lf\"/>";

	std::ifstream ifs(filename);

	char buffer[1024];

	std::vector<d4Vector> spheres(0);

	while (ifs.getline(buffer, 1024))
	{
		std::string line(buffer);

		if (ZIO::beginsWith(line, nextPointKey))
		{
			int id;
			double x, y, z, rad;

			if (std::sscanf(line.c_str(), formatStr.c_str(), &id, &x, &y, &z, &rad) != 5)
			{
				REPORT_ERROR_STR("Bad syntax in " << filename << ": " << line);
			}

			spheres.push_back(binning * d4Vector(x,y,z,rad));
		}
	}

	return spheres;
}

