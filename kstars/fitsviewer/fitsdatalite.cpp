/***************************************************************************
                          fitsdatalite.cpp  -  FITS Image
                             -------------------
    begin                : Fri Jul 22 2016
    copyright            : (C) 2016 by Jasem Mutlaq and Artem Fedoskin
    email                : mutlaqja@ikarustech.com, afedoskin3@gmail.com
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   Some code fragments were adapted from Peter Kirchgessner's FITS plugin*
 *   See http://members.aol.com/pkirchg for more details.                  *
 ***************************************************************************/

#include <config-kstars.h>
#include "fitsdatalite.h"

#include <cmath>
#include <cstdlib>
#include <climits>

#include <QApplication>
#include <QLocale>
#include <QFile>

#ifndef KSTARS_LITE
#include <KMessageBox>
#endif
#include <KLocalizedString>

#ifdef HAVE_WCSLIB
#include <wcshdr.h>
#include <wcsfix.h>
#include <wcs.h>
#endif

#include "ksutils.h"
#include "Options.h"

#define ZOOM_DEFAULT	100.0
#define ZOOM_MIN	10
#define ZOOM_MAX	400
#define ZOOM_LOW_INCR	10
#define ZOOM_HIGH_INCR	50

const int MINIMUM_ROWS_PER_CENTER=3;

#define DIFFUSE_THRESHOLD       0.15

#define MAX_EDGE_LIMIT  10000
#define LOW_EDGE_CUTOFF_1   50
#define LOW_EDGE_CUTOFF_2   10
#define MINIMUM_EDGE_LIMIT  2
#define SMALL_SCALE_SQUARE  256

bool greaterThan(Edge *s1, Edge *s2)
{
    //return s1->width > s2->width;
    return s1->sum > s2->sum;
}

FITSDataLite::FITSDataLite(FITSMode fitsMode)
{
    channels = 0;
    image_buffer = NULL;
    bayer_buffer = NULL;
    wcs_coord    = NULL;
    fptr = NULL;
    maxHFRStar = NULL;
    darkFrame = NULL;
    tempFile  = false;
    starsSearched = false;
    HasWCS = false;
    HasDebayer=false;
    mode = fitsMode;

    debayerParams.method  = DC1394_BAYER_METHOD_NEAREST;
    debayerParams.filter  = DC1394_COLOR_FILTER_RGGB;
    debayerParams.offsetX  = debayerParams.offsetY = 0;
}

FITSDataLite::~FITSDataLite()
{
    int status=0;

    clearImageBuffers();

    if (starCenters.count() > 0)
        qDeleteAll(starCenters);

    delete[] wcs_coord;

    if (fptr)
    {
        fits_close_file(fptr, &status);

        if (tempFile)
            QFile::remove(filename);

    }
}

bool FITSDataLite::loadFITS (const QString &inFilename, bool silent)
{
    int status=0, anynull=0;
    long naxes[3];
    char error_status[512];
    QString errMessage;

    qDeleteAll(starCenters);
    starCenters.clear();

    if (fptr)
    {
        fits_close_file(fptr, &status);

        if (tempFile)
            QFile::remove(filename);
    }

    filename = inFilename;

    if (filename.startsWith("/tmp/") || filename.contains("/Temp"))
        tempFile = true;
    else
        tempFile = false;

    if (fits_open_image(&fptr, filename.toLatin1(), READONLY, &status))
    {
        fits_report_error(stderr, status);
        fits_get_errstatus(status, error_status);
        errMessage = i18n("Could not open file %1. Error %2", filename, QString::fromUtf8(error_status));
        if (silent == false)
        if (Options::fITSLogging())
            qDebug() << errMessage;
        return false;
    }

    if (fits_get_img_param(fptr, 3, &(stats.bitpix), &(stats.ndim), naxes, &status))
    {
        fits_report_error(stderr, status);
        fits_get_errstatus(status, error_status);
        errMessage = i18n("FITS file open error (fits_get_img_param): %1", QString::fromUtf8(error_status));
        if (silent == false)
        if (Options::fITSLogging())
            qDebug() << errMessage;
        return false;
    }

    if (stats.ndim < 2)
    {
        errMessage = i18n("1D FITS images are not supported in KStars.");
        if (silent == false)
        if (Options::fITSLogging())
            qDebug() << errMessage;
        return false;
    }

    switch (stats.bitpix)
    {
    case 8:
        data_type = TBYTE;
        break;
    case 16:
        data_type = TUSHORT;
        break;
    case 32:
        data_type = TINT;
        break;
    case -32:
        data_type = TFLOAT;
        break;
    case 64:
        data_type = TLONGLONG;
        break;
    case -64:
        data_type = TDOUBLE;
    default:
        errMessage = i18n("Bit depth %1 is not supported.", stats.bitpix);
        if (silent == false)
        if (Options::fITSLogging())
            qDebug() << errMessage;
        return false;
        break;
    }

    if (stats.ndim < 3)
        naxes[2] = 1;

    if (naxes[0] == 0 || naxes[1] == 0)
    {
        errMessage = i18n("Image has invalid dimensions %1x%2", naxes[0], naxes[1]);
        if (silent == false)
        if (Options::fITSLogging())
            qDebug() << errMessage;
        return false;
    }

    stats.width  = naxes[0];
    stats.height = naxes[1];
    stats.samples_per_channel   = stats.width*stats.height;

    clearImageBuffers();

    channels = naxes[2];

    image_buffer = new float[stats.samples_per_channel * channels];
    if (image_buffer == NULL)
    {
        qDebug() << "FITSData: Not enough memory for image_buffer channel. Requested: " << stats.samples_per_channel * channels * sizeof(float) << " bytes.";
        clearImageBuffers();
        return false;
    }

    rotCounter=0;
    flipHCounter=0;
    flipVCounter=0;
    long nelements = stats.samples_per_channel * channels;

    if (fits_read_img(fptr, TFLOAT, 1, nelements, 0, image_buffer, &anynull, &status))
    {
        char errmsg[512];
        fits_get_errstatus(status, errmsg);
        errMessage = i18n("Error reading image: %1", QString(errmsg));
        if (silent == false) {
            qDebug() << errMessage;
        }
        fits_report_error(stderr, status);
        if (Options::fITSLogging())
            qDebug() << errMessage;
        return false;
    }

    if (darkFrame != NULL)
        subtract(darkFrame);

    if (darkFrame == NULL)
        calculateStats();

    if (mode == FITS_NORMAL)
        checkWCS();

    if (checkDebayer())
        debayer();

    starsSearched = false;

    return true;

}

int FITSDataLite::saveFITS( const QString &newFilename )
{
    int status=0, exttype=0;
    long nelements;
    fitsfile *new_fptr;

    nelements = stats.samples_per_channel * channels;

    /* Create a new File, overwriting existing*/
    if (fits_create_file(&new_fptr, newFilename.toLatin1(), &status))
    {
        fits_report_error(stderr, status);
        return status;
    }

    if (fits_movabs_hdu(fptr, 1, &exttype, &status))
    {
        fits_report_error(stderr, status);
        return status;
    }

    if (fits_copy_file(fptr, new_fptr, 1, 1, 1, &status))
    {
        fits_report_error(stderr, status);
        return status;
    }

    if (HasDebayer)
    {
        if (fits_close_file(fptr, &status))
        {
            fits_report_error(stderr, status);
            return status;
        }

        if (tempFile)
        {
            QFile::remove(filename);
            tempFile = false;
        }

        filename = newFilename;

        fptr = new_fptr;

        return 0;
    }

    /* close current file */
    if (fits_close_file(fptr, &status))
    {
        fits_report_error(stderr, status);
        return status;
    }

    status=0;

    if (tempFile)
    {
        QFile::remove(filename);
        tempFile = false;
    }

    filename = newFilename;

    fptr = new_fptr;

    if (fits_movabs_hdu(fptr, 1, &exttype, &status))
    {
        fits_report_error(stderr, status);
        return status;
    }

    /* Write Data */
    if (fits_write_img(fptr, TFLOAT, 1, nelements, image_buffer, &status))
    {
        fits_report_error(stderr, status);
        return status;
    }

    /* Write keywords */

    // Minimum
    if (fits_update_key(fptr, TDOUBLE, "DATAMIN", &(stats.min), "Minimum value", &status))
    {
        fits_report_error(stderr, status);
        return status;
    }

    // Maximum
    if (fits_update_key(fptr, TDOUBLE, "DATAMAX", &(stats.max), "Maximum value", &status))
    {
        fits_report_error(stderr, status);
        return status;
    }

    // NAXIS1
    if (fits_update_key(fptr, TUSHORT, "NAXIS1", &(stats.width), "length of data axis 1", &status))
    {
        fits_report_error(stderr, status);
        return status;
    }

    // NAXIS2
    if (fits_update_key(fptr, TUSHORT, "NAXIS2", &(stats.height), "length of data axis 2", &status))
    {
        fits_report_error(stderr, status);
        return status;
    }

    // ISO Date
    if (fits_write_date(fptr, &status))
    {
        fits_report_error(stderr, status);
        return status;
    }

    QString history = QString("Modified by KStars on %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-ddThh:mm:ss"));
    // History
    if (fits_write_history(fptr, history.toLatin1(), &status))
    {
        fits_report_error(stderr, status);
        return status;
    }

    int rot=0, mirror=0;
    if (rotCounter > 0)
    {
        rot = (90 * rotCounter) % 360;
        if (rot < 0)
            rot += 360;
    }
    if (flipHCounter %2 !=0 || flipVCounter % 2 != 0)
        mirror = 1;

    if (rot || mirror)
        rotWCSFITS(rot, mirror);

    rotCounter=flipHCounter=flipVCounter=0;

    return status;
}

void FITSDataLite::clearImageBuffers()
{
    delete[] image_buffer;
    image_buffer=NULL;
    delete [] bayer_buffer;
    bayer_buffer=NULL;
}

int FITSDataLite::calculateMinMax(bool refresh)
{
    int status, nfound=0;

    status = 0;

    if (refresh == false)
    {
        if (fits_read_key_dbl(fptr, "DATAMIN", &(stats.min[0]), NULL, &status) ==0)
            nfound++;

        if (fits_read_key_dbl(fptr, "DATAMAX", &(stats.max[0]), NULL, &status) == 0)
            nfound++;

        // If we found both keywords, no need to calculate them, unless they are both zeros
        if (nfound == 2 && !(stats.min[0] == 0 && stats.max[0] ==0))
            return 0;
    }

    stats.min[0]= 1.0E30;
    stats.max[0]= -1.0E30;

    stats.min[1]= 1.0E30;
    stats.max[1]= -1.0E30;

    stats.min[2]= 1.0E30;
    stats.max[2]= -1.0E30;

    if (channels == 1)
    {
        for (unsigned int i=0; i < stats.samples_per_channel; i++)
        {
            if (image_buffer[i] < stats.min[0]) stats.min[0] = image_buffer[i];
            else if (image_buffer[i] > stats.max[0]) stats.max[0] = image_buffer[i];
        }
    }
    else
    {
        int g_offset = stats.samples_per_channel;
        int b_offset = stats.samples_per_channel*2;

        for (unsigned int i=0; i < stats.samples_per_channel; i++)
        {
            if (image_buffer[i] < stats.min[0])
                stats.min[0] = image_buffer[i];
            else if (image_buffer[i] > stats.max[0])
                stats.max[0] = image_buffer[i];

            if (image_buffer[i+g_offset] < stats.min[1])
                stats.min[1] = image_buffer[i+g_offset];
            else if (image_buffer[i+g_offset] > stats.max[1])
                stats.max[1] = image_buffer[i+g_offset];

            if (image_buffer[i+b_offset] < stats.min[2])
                stats.min[2] = image_buffer[i+b_offset];
            else if (image_buffer[i+b_offset] > stats.max[2])
                stats.max[2] = image_buffer[i+b_offset];
        }
    }

    //qDebug() << "DATAMIN: " << stats.min << " - DATAMAX: " << stats.max;
    return 0;
}


void FITSDataLite::calculateStats(bool refresh)
{
    // Calculate min max
    calculateMinMax(refresh);

    // Get standard deviation and mean in one run
    runningAverageStdDev();

    stats.SNR = stats.mean[0] / stats.stddev[0];

    if (refresh && markStars)
        // Let's try to find star positions again after transformation
        starsSearched = false;

}

void FITSDataLite::runningAverageStdDev()
{
    int m_n = 2;
    double m_oldM=0, m_newM=0, m_oldS=0, m_newS=0;
    m_oldM = m_newM = image_buffer[0];

    for (unsigned int i=1; i < stats.samples_per_channel; i++)
    {
        m_newM = m_oldM + (image_buffer[i] - m_oldM)/m_n;
        m_newS = m_oldS + (image_buffer[i] - m_oldM) * (image_buffer[i] - m_newM);

        m_oldM = m_newM;
        m_oldS = m_newS;
        m_n++;
    }

    double variance = m_newS / (m_n -2);

    stats.mean[0] = m_newM;
    stats.stddev[0]  = sqrt(variance);
}

void FITSDataLite::setMinMax(double newMin,  double newMax, uint8_t channel)
{
    stats.min[channel] = newMin;
    stats.max[channel] = newMax;
}

int FITSDataLite::getFITSRecord(QString &recordList, int &nkeys)
{
    char *header;
    int status=0;

    if (fits_hdr2str(fptr, 0, NULL, 0, &header, &nkeys, &status))
    {
        fits_report_error(stderr, status);
        free(header);
        return -1;
    }

    recordList = QString(header);

    free(header);

    return 0;
}

bool FITSDataLite::checkCollision(Edge* s1, Edge*s2)
{
    int dis; //distance

    int diff_x=s1->x - s2->x;
    int diff_y=s1->y - s2->y;

    dis = std::abs( sqrt( diff_x*diff_x + diff_y*diff_y));
    dis -= s1->width/2;
    dis -= s2->width/2;

    if (dis<=0) //collision
        return true;

    //no collision
    return false;
}

double FITSDataLite::getHFR(HFRType type)
{
    // This method is less susceptible to noise
    // Get HFR for the brightest star only, instead of averaging all stars
    // It is more consistent.
    // TODO: Try to test this under using a real CCD.

    if (starCenters.size() == 0)
        return -1;

    if (type == HFR_MAX)
    {
        maxHFRStar = NULL;
        int maxVal=0;
        int maxIndex=0;
        for (int i=0; i < starCenters.count() ; i++)
        {
            if (starCenters[i]->val > maxVal)
            {
                maxIndex=i;
                maxVal = starCenters[i]->val;

            }
        }

        maxHFRStar = starCenters[maxIndex];
        return starCenters[maxIndex]->HFR;
    }

    double FSum=0;
    double avgHFR=0;

    // Weighted average HFR
    for (int i=0; i < starCenters.count() ; i++)
    {
        avgHFR += starCenters[i]->val * starCenters[i]->HFR;
        FSum   += starCenters[i]->val;
    }

    if (FSum != 0)
    {
        //qDebug() << "Average HFR is " << avgHFR / FSum << endl;
        return (avgHFR / FSum);
    }
    else
        return -1;

}

double FITSDataLite::getHFR(int x, int y)
{
    if (starCenters.size() == 0)
        return -1;

    for (int i=0; i < starCenters.count() ; i++)
    {
        if (fabs(starCenters[i]->x-x) <= starCenters[i]->width/2 && fabs(starCenters[i]->y-y) <= starCenters[i]->width/2)
        {
            return starCenters[i]->HFR;
        }
    }

    return -1;
}

void FITSDataLite::applyFilter(FITSScale type, float *image, float min, float max)
{
    /*if (type == FITS_NONE /* || histogram == NULL)
        return;

    double coeff=0;
    float val=0,bufferVal =0;
    int offset=0, row=0;


    if (image == NULL)
        image = image_buffer;

    int width = stats.width;
    int height = stats.height;

    if (min == -1)
        min = stats.min[0];
    if (max == -1)
        max = stats.max[0];

    int size = stats.samples_per_channel;
    int index=0;

    switch (type)
    {
    case FITS_AUTO:
    case FITS_LINEAR:
    {

        for (int i=0; i < channels; i++)
        {
            offset = i*size;
            for (int j=0; j < height; j++)
            {
                row = offset + j * width;
                for (int k=0; k < width; k++)
                {
                    index=k + row;
                    bufferVal = image[index];
                    if (bufferVal < min) bufferVal = min;
                    else if (bufferVal > max) bufferVal = max;
                    image_buffer[index] = bufferVal;
                }
            }
        }

        stats.min[0] = min;
        stats.max[0] = max;
        //runningAverageStdDev();
    }
        break;

    case FITS_LOG:
    {
        coeff = max / log(1 + max);

        for (int i=0; i < channels; i++)
        {
            offset = i*size;
            for (int j=0; j < height; j++)
            {
                row = offset + j * width;
                for (int k=0; k < width; k++)
                {
                    index=k + row;
                    bufferVal = image[index];
                    if (bufferVal < min) bufferVal = min;
                    else if (bufferVal > max) bufferVal = max;
                    val = (coeff * log(1 + qBound(min, image[index], max)));
                    image_buffer[index] = qBound(min, val, max);
                }
            }

        }

        stats.min[0] = min;
        stats.max[0] = max;
        runningAverageStdDev();
    }
        break;

    case FITS_SQRT:
    {
        coeff = max / sqrt(max);

        for (int i=0; i < channels; i++)
        {
            offset = i*size;
            for (int j=0; j < height; j++)
            {
                row = offset + j * width;
                for (int k=0; k < width; k++)
                {
                    index=k + row;
                    val = (int) (coeff * sqrt(qBound(min, image[index], max)));
                    image_buffer[index] = val;
                }
            }
        }

        stats.min[0] = min;
        stats.max[0] = max;
        runningAverageStdDev();
    }
        break;

    case FITS_AUTO_STRETCH:
    {
        min = stats.mean[0] - stats.stddev[0];
        max = stats.mean[0] + stats.stddev[0] * 3;

        for (int i=0; i < channels; i++)
        {
            offset = i*size;
            for (int j=0; j < height; j++)
            {
                row = offset + j * width;
                for (int k=0; k < width; k++)
                {
                    index=k + row;
                    image_buffer[index] = qBound(min, image[index], max);
                }
            }
        }

        stats.min[0] = min;
        stats.max[0] = max;
        runningAverageStdDev();
    }
        break;

    case FITS_HIGH_CONTRAST:
    {
        min = stats.mean[0] + stats.stddev[0];
        if (min < 0)
            min =0;
        max = stats.mean[0] + stats.stddev[0] * 3;

        for (int i=0; i < channels; i++)
        {
            offset = i*size;
            for (int j=0; j < height; j++)
            {
                row = offset + j * width;
                for (int k=0; k < width; k++)
                {
                    index=k + row;
                    image_buffer[index] = qBound(min, image[index], max);
                }
            }
        }
        stats.min[0] = min;
        stats.max[0] = max;
        runningAverageStdDev();
    }
        break;

    case FITS_EQUALIZE:
    {
        if (histogram == NULL)
            return;
        QVector<double> cumulativeFreq = histogram->getCumulativeFrequency();
        coeff = 255.0 / (height * width);

        for (int i=0; i < channels; i++)
        {
            offset = i*size;
            for (int j=0; j < height; j++)
            {
                row = offset + j * width;
                for (int k=0; k < width; k++)
                {
                    index=k + row;
                    bufferVal = (int) (image[index] - min) / histogram->getBinWidth();

                    if (bufferVal >= cumulativeFreq.size())
                        bufferVal = cumulativeFreq.size()-1;

                    val = (int) (coeff * cumulativeFreq[bufferVal]);

                    image_buffer[index] = val;
                }
            }
        }
    }
        calculateStats(true);
        break;

    case FITS_HIGH_PASS:
    {
        min = stats.mean[0];
        for (int i=0; i < channels; i++)
        {
            offset = i*size;
            for (int j=0; j < height; j++)
            {
                row = offset + j * width;
                for (int k=0; k < width; k++)
                {
                    index=k + row;
                    image_buffer[index] = qBound(min, image[index], max);
                }
            }
        }

        stats.min[0] = min;
        stats.max[0] = max;
        runningAverageStdDev();
    }
        break;

    // Based on http://www.librow.com/articles/article-1
    case FITS_MEDIAN:
    {
        float* extension = new float[(width + 2) * (height + 2)];
        //   Check memory allocation
        if (!extension)
            return;
        //   Create image extension
        for (int ch=0; ch < channels; ch++)
        {
            offset = ch*size;
            int N=width,M=height;

            for (int i = 0; i < M; ++i)
            {
                memcpy(extension + (N + 2) * (i + 1) + 1, image_buffer + N * i + offset, N * sizeof(float));
                extension[(N + 2) * (i + 1)] = image_buffer[N * i + offset];
                extension[(N + 2) * (i + 2) - 1] = image_buffer[N * (i + 1) - 1 + offset];
            }
            //   Fill first line of image extension
            memcpy(extension, extension + N + 2, (N + 2) * sizeof(float));
            //   Fill last line of image extension
            memcpy(extension + (N + 2) * (M + 1), extension + (N + 2) * M, (N + 2) * sizeof(float));
            //   Call median filter implementation

            N=width+2;
            M=height+2;
            //   Move window through all elements of the image
            for (int m = 1; m < M - 1; ++m)
                for (int n = 1; n < N - 1; ++n)
                {
                    //   Pick up window elements
                    int k = 0;
                    float window[9];
                    for (int j = m - 1; j < m + 2; ++j)
                        for (int i = n - 1; i < n + 2; ++i)
                            window[k++] = extension[j * N + i];
                    //   Order elements (only half of them)
                    for (int j = 0; j < 5; ++j)
                    {
                        //   Find position of minimum element
                        int mine = j;
                        for (int l = j + 1; l < 9; ++l)
                            if (window[l] < window[mine])
                                mine = l;
                        //   Put found minimum element in its place
                        const float temp = window[j];
                        window[j] = window[mine];
                        window[mine] = temp;
                    }
                    //   Get result - the middle element
                    image_buffer[(m - 1) * (N - 2) + n - 1 + offset] = window[4];
                }
        }

        //   Free memory
        delete[] extension;
        runningAverageStdDev();
    }
        break;


    case FITS_ROTATE_CW:
        rotFITS(90, 0);
        rotCounter++;
        break;

    case FITS_ROTATE_CCW:
        rotFITS(270, 0);
        rotCounter--;
        break;

    case FITS_FLIP_H:
        rotFITS(0, 1);
        flipHCounter++;
        break;

    case FITS_FLIP_V:
        rotFITS(0, 2);
        flipVCounter++;
        break;

    case FITS_CUSTOM:
    default:
        return;
        break;
    }
*/
}

void FITSDataLite::subtract(float *dark_buffer)
{
    for (int i=0; i < stats.width*stats.height; i++)
    {
        image_buffer[i] -= dark_buffer[i];
        if (image_buffer[i] < 0)
            image_buffer[i] = 0;
    }

    calculateStats(true);
}

int FITSDataLite::findStars(const QRectF &boundary, bool force)
{
    /*if (histogram == NULL)
        return -1;

    if (starsSearched == false || force)
    {
        qDeleteAll(starCenters);
        starCenters.clear();

        findCentroid(boundary);
        getHFR();
    }

    starsSearched = true;

    return starCenters.count();
*/
    return -1;
}

void FITSDataLite::getCenterSelection(int *x, int *y)
{
    if (starCenters.count() == 0)
        return;

    Edge *pEdge = new Edge();
    pEdge->x = *x;
    pEdge->y = *y;
    pEdge->width = 1;

    foreach(Edge *center, starCenters)
        if (checkCollision(pEdge, center))
        {
            *x = center->x;
            *y = center->y;
            break;
        }

    delete (pEdge);
}

void FITSDataLite::checkWCS()
{
#ifdef HAVE_WCSLIB

    int status=0;
    char *header;
    int nkeyrec, nreject, nwcs, stat[2];
    double imgcrd[2], phi, pixcrd[2], theta, world[2];
    struct wcsprm *wcs=0;
    int width=getWidth();
    int height=getHeight();

    if (fits_hdr2str(fptr, 1, NULL, 0, &header, &nkeyrec, &status))
    {
        fits_report_error(stderr, status);
        return;
    }

    if ((status = wcspih(header, nkeyrec, WCSHDR_all, -3, &nreject, &nwcs, &wcs)))
    {
        fprintf(stderr, "wcspih ERROR %d: %s.\n", status, wcshdr_errmsg[status]);
        return;
    }

    free(header);

    if (wcs == 0)
    {
        //fprintf(stderr, "No world coordinate systems found.\n");
        return;
    }

    // FIXME: Call above goes through EVEN if no WCS is present, so we're adding this to return for now.
    if (wcs->crpix[0] == 0)
        return;

    HasWCS = true;

    if ((status = wcsset(wcs)))
    {
        fprintf(stderr, "wcsset ERROR %d: %s.\n", status, wcs_errmsg[status]);
        return;
    }

    delete[] wcs_coord;

    wcs_coord = new wcs_point[width*height];

    wcs_point *p = wcs_coord;

    for (int i=0; i < height; i++)
    {
        for (int j=0; j < width; j++)
        {
            pixcrd[0]=j;
            pixcrd[1]=i;

            if ((status = wcsp2s(wcs, 1, 2, &pixcrd[0], &imgcrd[0], &phi, &theta, &world[0], &stat[0])))
            {
                fprintf(stderr, "wcsp2s ERROR %d: %s.\n", status,
                        wcs_errmsg[status]);
            }
            else
            {
                p->ra  = world[0];
                p->dec = world[1];

                p++;
            }
        }
    }
#endif

}
float *FITSDataLite::getDarkFrame() const
{
    return darkFrame;
}

void FITSDataLite::setDarkFrame(float *value)
{
    darkFrame = value;
}

int FITSDataLite::getFlipVCounter() const
{
    return flipVCounter;
}

void FITSDataLite::setFlipVCounter(int value)
{
    flipVCounter = value;
}

int FITSDataLite::getFlipHCounter() const
{
    return flipHCounter;
}

void FITSDataLite::setFlipHCounter(int value)
{
    flipHCounter = value;
}

int FITSDataLite::getRotCounter() const
{
    return rotCounter;
}

void FITSDataLite::setRotCounter(int value)
{
    rotCounter = value;
}


/* Rotate an image by 90, 180, or 270 degrees, with an optional
 * reflection across the vertical or horizontal axis.
 * verbose generates extra info on stdout.
 * return NULL if successful or rotated image.
 */
bool FITSDataLite::rotFITS (int rotate, int mirror)
{
    int ny, nx;
    int x1, y1, x2, y2;
    float *rotimage = NULL;
    int offset=0;

    if (rotate == 1)
        rotate = 90;
    else if (rotate == 2)
        rotate = 180;
    else if (rotate == 3)
        rotate = 270;
    else if (rotate < 0)
        rotate = rotate + 360;

    nx = stats.width;
    ny = stats.height;

    /* Allocate buffer for rotated image */
    rotimage = new float[stats.samples_per_channel*channels];
    if (rotimage == NULL)
    {
        qWarning() << "Unable to allocate memory for rotated image buffer!";
        return false;
    }

    /* Mirror image without rotation */
    if (rotate < 45 && rotate > -45)
    {
        if (mirror == 1)
        {
            for (int i=0; i < channels; i++)
            {
                offset = stats.samples_per_channel * i;
                for (x1 = 0; x1 < nx; x1++)
                {
                    x2 = nx - x1 - 1;
                    for (y1 = 0; y1 < ny; y1++)
                        rotimage[(y1*nx) + x2 + offset] = image_buffer[(y1*nx) + x1 + offset];
                }
            }

        }
        else if (mirror == 2)
        {
            for (int i=0; i < channels; i++)
            {
                offset = stats.samples_per_channel * i;
                for (y1 = 0; y1 < ny; y1++)
                {
                    y2 = ny - y1 - 1;
                    for (x1 = 0; x1 < nx; x1++)
                        rotimage[(y2*nx) + x1 + offset] = image_buffer[(y1*nx) + x1 + offset];
                }
            }

        }
        else
        {
            for (int i=0; i < channels; i++)
            {
                offset = stats.samples_per_channel * i;
                for (y1 = 0; y1 < ny; y1++)
                {
                    for (x1 = 0; x1 < nx; x1++)
                        rotimage[(y1*nx) + x1 + offset] = image_buffer[(y1*nx) + x1 + offset];
                }
            }

        }
    }

    /* Rotate by 90 degrees */
    else if (rotate >= 45 && rotate < 135)
    {
        if (mirror == 1)
        {
            for (int i=0; i < channels; i++)
            {
                offset = stats.samples_per_channel * i;
                for (y1 = 0; y1 < ny; y1++)
                {
                    x2 = ny - y1 - 1;
                    for (x1 = 0; x1 < nx; x1++)
                    {
                        y2 = nx - x1 - 1;
                        rotimage[(y2*ny) + x2 + offset] = image_buffer[(y1*nx) + x1 + offset];
                    }
                }
            }

        }
        else if (mirror == 2)
        {
            for (int i=0; i < channels; i++)
            {
                offset = stats.samples_per_channel * i;
                for (y1 = 0; y1 < ny; y1++)
                {
                    for (x1 = 0; x1 < nx; x1++)
                        rotimage[(x1*ny) + y1 + offset] = image_buffer[(y1*nx) + x1 + offset];
                }
            }

        }
        else
        {
            for (int i=0; i < channels; i++)
            {
                offset = stats.samples_per_channel * i;
                for (y1 = 0; y1 < ny; y1++)
                {
                    x2 = ny - y1 - 1;
                    for (x1 = 0; x1 < nx; x1++)
                    {
                        y2 = x1;
                        rotimage[(y2*ny) + x2 + offset] = image_buffer[(y1*nx) + x1 + offset];
                    }
                }
            }

        }

        stats.width = ny;
        stats.height = nx;
    }

    /* Rotate by 180 degrees */
    else if (rotate >= 135 && rotate < 225)
    {
        if (mirror == 1)
        {
            for (int i=0; i < channels; i++)
            {
                offset = stats.samples_per_channel * i;
                for (y1 = 0; y1 < ny; y1++)
                {
                    y2 = ny - y1 - 1;
                    for (x1 = 0; x1 < nx; x1++)
                        rotimage[(y2*nx) + x1 + offset] = image_buffer[(y1*nx) + x1 + offset];
                }
            }

        }
        else if (mirror == 2)
        {
            for (int i=0; i < channels; i++)
            {
                offset = stats.samples_per_channel * i;
                for (x1 = 0; x1 < nx; x1++)
                {
                    x2 = nx - x1 - 1;
                    for (y1 = 0; y1 < ny; y1++)
                        rotimage[(y1*nx) + x2 + offset] = image_buffer[(y1*nx) + x1 + offset];
                }
            }
        }
        else
        {
            for (int i=0; i < channels; i++)
            {
                offset = stats.samples_per_channel * i;
                for (y1 = 0; y1 < ny; y1++)
                {
                    y2 = ny - y1 - 1;
                    for (x1 = 0; x1 < nx; x1++)
                    {
                        x2 = nx - x1 - 1;
                        rotimage[(y2*nx) + x2 + offset] = image_buffer[(y1*nx) + x1 + offset];
                    }
                }
            }
        }
    }

    /* Rotate by 270 degrees */
    else if (rotate >= 225 && rotate < 315)
    {
        if (mirror == 1)
        {
            for (int i=0; i < channels; i++)
            {
                offset = stats.samples_per_channel * i;
                for (y1 = 0; y1 < ny; y1++)
                {
                    for (x1 = 0; x1 < nx; x1++)
                        rotimage[(x1*ny) + y1 + offset] = image_buffer[(y1*nx) + x1 + offset];
                }
            }
        }
        else if (mirror == 2)
        {
            for (int i=0; i < channels; i++)
            {
                offset = stats.samples_per_channel * i;
                for (y1 = 0; y1 < ny; y1++)
                {
                    x2 = ny - y1 - 1;
                    for (x1 = 0; x1 < nx; x1++)
                    {
                        y2 = nx - x1 - 1;
                        rotimage[(y2*ny) + x2 + offset] = image_buffer[(y1*nx) + x1 + offset];
                    }
                }
            }
        }
        else
        {
            for (int i=0; i < channels; i++)
            {
                offset = stats.samples_per_channel * i;
                for (y1 = 0; y1 < ny; y1++)
                {
                    x2 = y1;
                    for (x1 = 0; x1 < nx; x1++)
                    {
                        y2 = nx - x1 - 1;
                        rotimage[(y2*ny) + x2 + offset] = image_buffer[(y1*nx) + x1 + offset];
                    }
                }
            }
        }

        stats.width = ny;
        stats.height = nx;
    }

    /* If rotating by more than 315 degrees, assume top-bottom reflection */
    else if (rotate >= 315 && mirror)
    {
        for (int i=0; i < channels; i++)
        {
            offset = stats.samples_per_channel * i;
            for (y1 = 0; y1 < ny; y1++)
            {
                for (x1 = 0; x1 < nx; x1++)
                {
                    x2 = y1;
                    y2 = x1;
                    rotimage[(y2*ny) + x2 + offset] = image_buffer[(y1*nx) + x1 + offset];
                }
            }
        }
    }

    delete[] image_buffer;
    image_buffer = rotimage;

    return true;
}

void FITSDataLite::rotWCSFITS (int angle, int mirror)
{
    int status=0;
    char comment[100];
    double ctemp1, ctemp2, ctemp3, ctemp4, naxis1, naxis2;
    int WCS_DECIMALS=6;

    naxis1=stats.width;
    naxis2=stats.height;

    if (fits_read_key_dbl(fptr, "CD1_1", &ctemp1, comment, &status ))
    {
        // No WCS keywords
        return;
    }

    /* Reset CROTAn and CD matrix if axes have been exchanged */
    if (angle == 90)
    {
        if (!fits_read_key_dbl(fptr, "CROTA1", &ctemp1, comment, &status ))
            fits_update_key_dbl(fptr, "CROTA1", ctemp1+90.0, WCS_DECIMALS, comment, &status );

        if (!fits_read_key_dbl(fptr, "CROTA2", &ctemp1, comment, &status ))
            fits_update_key_dbl(fptr, "CROTA2", ctemp1+90.0, WCS_DECIMALS, comment, &status );
    }

    status=0;

    /* Negate rotation angle if mirrored */
    if (mirror)
    {
        if (!fits_read_key_dbl(fptr, "CROTA1", &ctemp1, comment, &status ))
            fits_update_key_dbl(fptr, "CROTA1", -ctemp1, WCS_DECIMALS, comment, &status );

        if (!fits_read_key_dbl(fptr, "CROTA2", &ctemp1, comment, &status ))
            fits_update_key_dbl(fptr, "CROTA2", -ctemp1, WCS_DECIMALS, comment, &status );

        status=0;

        if (!fits_read_key_dbl(fptr, "LTM1_1", &ctemp1, comment, &status ))
            fits_update_key_dbl(fptr, "LTM1_1", -ctemp1, WCS_DECIMALS, comment, &status );

        status=0;

        if (!fits_read_key_dbl(fptr, "CD1_1", &ctemp1, comment, &status ))
            fits_update_key_dbl(fptr, "CD1_1", -ctemp1, WCS_DECIMALS, comment, &status );

        if (!fits_read_key_dbl(fptr, "CD1_2", &ctemp1, comment, &status ))
            fits_update_key_dbl(fptr, "CD1_2", -ctemp1, WCS_DECIMALS, comment, &status );

        if (!fits_read_key_dbl(fptr, "CD2_1", &ctemp1, comment, &status ))
            fits_update_key_dbl(fptr, "CD2_1", -ctemp1, WCS_DECIMALS, comment, &status );
    }

    status=0;

    /* Unbin CRPIX and CD matrix */
    if (!fits_read_key_dbl(fptr, "LTM1_1", &ctemp1, comment, &status ))
    {
        if (ctemp1 != 1.0)
        {
            if (!fits_read_key_dbl(fptr, "LTM2_2", &ctemp2, comment, &status ))
                if (ctemp1 == ctemp2)
                {
                    double ltv1 = 0.0;
                    double ltv2 = 0.0;
                    status=0;
                    if (!fits_read_key_dbl(fptr, "LTV1", &ltv1, comment, &status))
                        fits_delete_key(fptr, "LTV1", &status);
                    if (!fits_read_key_dbl(fptr, "LTV2", &ltv2, comment, &status))
                        fits_delete_key(fptr, "LTV2", &status);

                    status=0;

                    if (!fits_read_key_dbl(fptr, "CRPIX1", &ctemp3, comment, &status ))
                        fits_update_key_dbl(fptr, "CRPIX1", (ctemp3-ltv1)/ctemp1, WCS_DECIMALS, comment, &status );

                    if (!fits_read_key_dbl(fptr, "CRPIX2", &ctemp3, comment, &status ))
                        fits_update_key_dbl(fptr, "CRPIX2", (ctemp3-ltv2)/ctemp1, WCS_DECIMALS, comment, &status );

                    status=0;

                    if (!fits_read_key_dbl(fptr, "CD1_1", &ctemp3, comment, &status ))
                        fits_update_key_dbl(fptr, "CD1_1", ctemp3/ctemp1, WCS_DECIMALS, comment, &status );

                    if (!fits_read_key_dbl(fptr, "CD1_2", &ctemp3, comment, &status ))
                        fits_update_key_dbl(fptr, "CD1_2", ctemp3/ctemp1, WCS_DECIMALS, comment, &status );

                    if (!fits_read_key_dbl(fptr, "CD2_1", &ctemp3, comment, &status ))
                        fits_update_key_dbl(fptr, "CD2_1", ctemp3/ctemp1, WCS_DECIMALS, comment, &status );

                    if (!fits_read_key_dbl(fptr, "CD2_2", &ctemp3, comment, &status ))
                        fits_update_key_dbl(fptr, "CD2_2", ctemp3/ctemp1, WCS_DECIMALS, comment, &status );

                    status=0;

                    fits_delete_key(fptr, "LTM1_1", &status);
                    fits_delete_key(fptr, "LTM1_2", &status);
                }
        }
    }

    status=0;

    /* Reset CRPIXn */
    if ( !fits_read_key_dbl(fptr, "CRPIX1", &ctemp1, comment, &status ) && !fits_read_key_dbl(fptr, "CRPIX2", &ctemp2, comment, &status ) )
    {
        if (mirror)
        {
            if (angle == 0)
                fits_update_key_dbl(fptr, "CRPIX1", naxis1-ctemp1, WCS_DECIMALS, comment, &status );
            else if (angle == 90)
            {
                fits_update_key_dbl(fptr, "CRPIX1", naxis2-ctemp2, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CRPIX2", naxis1-ctemp1, WCS_DECIMALS, comment, &status );
            }
            else if (angle == 180)
            {
                fits_update_key_dbl(fptr, "CRPIX1", ctemp1, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CRPIX2", naxis2-ctemp2, WCS_DECIMALS, comment, &status );
            }
            else if (angle == 270)
            {
                fits_update_key_dbl(fptr, "CRPIX1", ctemp2, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CRPIX2", ctemp1, WCS_DECIMALS, comment, &status );
            }
        }
        else
        {
            if (angle == 90)
            {
                fits_update_key_dbl(fptr, "CRPIX1", naxis2-ctemp2, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CRPIX2", ctemp1, WCS_DECIMALS, comment, &status );
            }
            else if (angle == 180)
            {
                fits_update_key_dbl(fptr, "CRPIX1", naxis1-ctemp1, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CRPIX2", naxis2-ctemp2, WCS_DECIMALS, comment, &status );
            }
            else if (angle == 270)
            {
                fits_update_key_dbl(fptr, "CRPIX1", ctemp2, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CRPIX2", naxis1-ctemp1, WCS_DECIMALS, comment, &status );
            }
        }
    }

    status=0;

    /* Reset CDELTn (degrees per pixel) */
    if ( !fits_read_key_dbl(fptr, "CDELT1", &ctemp1, comment, &status ) && !fits_read_key_dbl(fptr, "CDELT2", &ctemp2, comment, &status ) )
    {
        if (mirror)
        {
            if (angle == 0)
                fits_update_key_dbl(fptr, "CDELT1", -ctemp1, WCS_DECIMALS, comment, &status );
            else if (angle == 90)
            {
                fits_update_key_dbl(fptr, "CDELT1", -ctemp2, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CDELT2", -ctemp1, WCS_DECIMALS, comment, &status );

            }
            else if (angle == 180)
            {
                fits_update_key_dbl(fptr, "CDELT1", ctemp1, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CDELT2", -ctemp2, WCS_DECIMALS, comment, &status );
            }
            else if (angle == 270)
            {
                fits_update_key_dbl(fptr, "CDELT1", ctemp2, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CDELT2", ctemp1, WCS_DECIMALS, comment, &status );
            }
        }
        else
        {
            if (angle == 90)
            {
                fits_update_key_dbl(fptr, "CDELT1", -ctemp2, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CDELT2", ctemp1, WCS_DECIMALS, comment, &status );

            }
            else if (angle == 180)
            {
                fits_update_key_dbl(fptr, "CDELT1", -ctemp1, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CDELT2", -ctemp2, WCS_DECIMALS, comment, &status );
            }
            else if (angle == 270)
            {
                fits_update_key_dbl(fptr, "CDELT1", ctemp2, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CDELT2", -ctemp1, WCS_DECIMALS, comment, &status );
            }
        }
    }

    /* Reset CD matrix, if present */
    ctemp1 = 0.0;
    ctemp2 = 0.0;
    ctemp3 = 0.0;
    ctemp4 = 0.0;
    status=0;
    if ( !fits_read_key_dbl(fptr, "CD1_1", &ctemp1, comment, &status ) )
    {
        fits_read_key_dbl(fptr, "CD1_2", &ctemp2, comment, &status );
        fits_read_key_dbl(fptr, "CD2_1", &ctemp3, comment, &status );
        fits_read_key_dbl(fptr, "CD2_2", &ctemp4, comment, &status );
        status=0;
        if (mirror)
        {
            if (angle == 0)
            {
                fits_update_key_dbl(fptr, "CD1_2", -ctemp2, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD2_1", -ctemp3, WCS_DECIMALS, comment, &status );
            }
            else if (angle == 90)
            {
                fits_update_key_dbl(fptr, "CD1_1", -ctemp4, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD1_2", -ctemp3, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD2_1", -ctemp2, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD2_2", -ctemp1, WCS_DECIMALS, comment, &status );

            }
            else if (angle == 180)
            {
                fits_update_key_dbl(fptr, "CD1_1", ctemp1, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD1_2", ctemp2, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD2_1", -ctemp3, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD2_2", -ctemp4, WCS_DECIMALS, comment, &status );
            }
            else if (angle == 270)
            {
                fits_update_key_dbl(fptr, "CD1_1", ctemp4, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD1_2", ctemp3, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD2_1", ctemp3, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD2_2", ctemp1, WCS_DECIMALS, comment, &status );
            }
        }
        else {
            if (angle == 90)
            {
                fits_update_key_dbl(fptr, "CD1_1", -ctemp4, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD1_2", -ctemp3, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD2_1", ctemp1, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD2_2", ctemp1, WCS_DECIMALS, comment, &status );
            }
            else if (angle == 180)
            {
                fits_update_key_dbl(fptr, "CD1_1", -ctemp1, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD1_2", -ctemp2, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD2_1", -ctemp3, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD2_2", -ctemp4, WCS_DECIMALS, comment, &status );
            }
            else if (angle == 270)
            {
                fits_update_key_dbl(fptr, "CD1_1", ctemp4, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD1_2", ctemp3, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD2_1", -ctemp2, WCS_DECIMALS, comment, &status );
                fits_update_key_dbl(fptr, "CD2_2", -ctemp1, WCS_DECIMALS, comment, &status );
            }
        }
    }

    /* Delete any polynomial solution */
    /* (These could maybe be switched, but I don't want to work them out yet */
    status=0;
    if ( !fits_read_key_dbl(fptr, "CO1_1", &ctemp1, comment, &status ) )
    {
        int i;
        char keyword[16];

        for (i = 1; i < 13; i++)
        {
            sprintf (keyword,"CO1_%d", i);
            fits_delete_key(fptr, keyword, &status);
        }
        for (i = 1; i < 13; i++)
        {
            sprintf (keyword,"CO2_%d", i);
            fits_delete_key(fptr, keyword, &status);
        }
    }

    return;
}

float * FITSDataLite::getImageBuffer()
{
    return image_buffer;
}

void FITSDataLite::setImageBuffer(float *buffer)
{
    delete[] image_buffer;
    image_buffer = buffer;
}

bool FITSDataLite::checkDebayer()
{

    int status=0;
    char bayerPattern[64];

    // Let's search for BAYERPAT keyword, if it's not found we return as there is no bayer pattern in this image
    if (fits_read_keyword(fptr, "BAYERPAT", bayerPattern, NULL, &status))
        return false;

    if (stats.bitpix != 16 && stats.bitpix != 8)
    {
        //KMessageBox::error(NULL, i18n("Only 8 and 16 bits bayered images supported."), i18n("Debayer error"));
        return false;
    }
    QString pattern(bayerPattern);
    pattern = pattern.remove("'").trimmed();

    if (pattern == "RGGB")
        debayerParams.filter = DC1394_COLOR_FILTER_RGGB;
    else if (pattern == "GBRG")
        debayerParams.filter = DC1394_COLOR_FILTER_GBRG;
    else if (pattern == "GRBG")
        debayerParams.filter = DC1394_COLOR_FILTER_GRBG;
    else if (pattern == "BGGR")
        debayerParams.filter = DC1394_COLOR_FILTER_BGGR;
    // We return unless we find a valid pattern
    else
        return false;

    fits_read_key(fptr, TINT, "XBAYROFF", &debayerParams.offsetX, NULL, &status);
    fits_read_key(fptr, TINT, "YBAYROFF", &debayerParams.offsetY, NULL, &status);

    delete[] bayer_buffer;
    bayer_buffer = new float[stats.samples_per_channel * channels];
    if (bayer_buffer == NULL)
    {
        //KMessageBox::error(NULL, i18n("Unable to allocate memory for bayer buffer."), i18n("Open FITS"));
        return false;
    }
    memcpy(bayer_buffer, image_buffer, stats.samples_per_channel * channels * sizeof(float));

    HasDebayer = true;

    return true;

}

void FITSDataLite::getBayerParams(BayerParams *param)
{
    param->method       = debayerParams.method;
    param->filter       = debayerParams.filter;
    param->offsetX      = debayerParams.offsetX;
    param->offsetY      = debayerParams.offsetY;
}

void FITSDataLite::setBayerParams(BayerParams *param)
{
    debayerParams.method   = param->method;
    debayerParams.filter   = param->filter;
    debayerParams.offsetX  = param->offsetX;
    debayerParams.offsetY  = param->offsetY;
}

bool FITSDataLite::debayer()
{
    dc1394error_t error_code;

    int rgb_size = stats.samples_per_channel*3;
    float * dst = new float[rgb_size];
    if (dst == NULL)
    {
        //KMessageBox::error(NULL, i18n("Unable to allocate memory for temporary bayer buffer."), i18n("Debayer Error"));
        return false;
    }

    if ( (error_code = dc1394_bayer_decoding_float(bayer_buffer, dst, stats.width, stats.height, debayerParams.offsetX, debayerParams.offsetY,
                                                   debayerParams.filter, debayerParams.method)) != DC1394_SUCCESS)
    {
        //KMessageBox::error(NULL, i18n("Debayer failed (%1)", error_code), i18n("Debayer error"));
        channels=1;
        delete[] dst;
        //Restore buffer
        delete[] image_buffer;
        image_buffer = new float[stats.samples_per_channel];
        memcpy(image_buffer, bayer_buffer, stats.samples_per_channel * sizeof(float));
        return false;
    }

    if (channels == 1)
    {
        delete[] image_buffer;
        image_buffer = new float[rgb_size];

        if (image_buffer == NULL)
        {
            delete[] dst;
            //KMessageBox::error(NULL, i18n("Unable to allocate memory for debayerd buffer."), i18n("Debayer Error"));
            return false;
        }
    }

    // Data in R1G1B1, we need to copy them into 3 layers for FITS
    float * rBuff = image_buffer;
    float * gBuff = image_buffer + (stats.width * stats.height);
    float * bBuff = image_buffer + (stats.width * stats.height * 2);

    int imax = stats.samples_per_channel*3 - 3;
    for (int i=0; i <= imax; i += 3)
    {
        *rBuff++ = dst[i];
        *gBuff++ = dst[i+1];
        *bBuff++ = dst[i+2];
    }

    channels=3;
    delete[] dst;
    return true;

}

double FITSDataLite::getADU()
{
    double adu=0;
    for (int i=0; i < channels; i++)
        adu += stats.mean[i];

    return (adu/ (double) channels);
}

