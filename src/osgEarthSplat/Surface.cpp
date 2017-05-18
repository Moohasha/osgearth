/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2016 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include "Surface"
#include "SplatCatalog"
#include "SplatShaders"
#include <osgEarth/Map>
#include <osgEarth/ShaderGenerator>
#include <osgDB/Options>
#include <osg/TextureBuffer>

using namespace osgEarth;
using namespace osgEarth::Splat;

#define LC "[Surface] "

Surface::Surface()
{
    //nop
}

bool
Surface::configure(const ConfigOptions& conf, const Map* map, const osgDB::Options* dbo)
{
    SurfaceOptions in(conf);

    if (_catalog.valid() == false && in.catalogURI().isSet())
    {
        // Read in the catalog.
        _catalog = SplatCatalog::read( in.catalogURI().get(), dbo );
    }

    if ( !_catalog.valid() )
    {
        OE_WARN << LC << "Failed to read catalog for surface\n";
        return false;
    }
    
    return true;
}

bool
Surface::loadTextures(const LandCoverDictionary* landCoverDict, const osgDB::Options* dbo)
{
    int numValidTextures = 0;

    if ( landCoverDict == 0L || !_catalog.valid() )
        return false;

    if ( _catalog->createSplatTextureDef(dbo, _textureDef) )
    {
        _textureDef._splatLUTBuffer = createLUTBuffer(landCoverDict);
    }
    else
    {
        OE_WARN << LC << "Failed to create a texture for a catalog (" << _catalog->name().get() << ")\n";
        return false;
    }

    return true;
}

namespace
{
#define INDENTATION 4
    struct indent {
        indent(int level) :_level(level){}
        int _level;
        friend std::ostream& operator<<(std::ostream& os, const indent& val) {
            for (int i=0; i<val._level * INDENTATION; ++i) 
                os << ' ';
            return os;
        }
    };

    void write(std::ostream& buf, const SplatRangeData* rangeData, int I)
    {
        buf << indent(I) << "primary = " << (rangeData->_textureIndex) << ".0;\n";
    }
}

#define NUM_FLOATS_PER_LOD 6
#define NUM_LODS 26
#define NUM_CLASSES 256

namespace
{
    struct LOD {
        LOD() : primary(-1.0f), detail(-1.0f), brightness(1.0f), contrast(1.0f), threshold(0.0f), slope(0.0f) { }
        float primary, detail, brightness, contrast, threshold, slope;
    };

    void write(LOD& lod, const SplatRangeData& data)
    {
        lod.primary = (float)data._textureIndex;
        if (data._detail.isSet())
        {
            lod.detail = (float)data._detail->_textureIndex;
            if (data._detail->_brightness.isSet())
                lod.brightness = data._detail->_brightness.get();
            if (data._detail->_contrast.isSet())
                lod.contrast = data._detail->_contrast.get();
            if (data._detail->_threshold.isSet())
                lod.threshold = data._detail->_threshold.get();
            if (data._detail->_slope.isSet())
                lod.slope = data._detail->_slope.get();
        }
    }
}

osg::Texture*
Surface::createLUTBuffer(const LandCoverDictionary* landCoverDict) const
{
    typedef LOD CoverageClass[NUM_LODS];

    typedef CoverageClass LUT[NUM_CLASSES];

    LUT lut;

    for(LandCoverClassVector::const_iterator i = landCoverDict->getClasses().begin();
        i != landCoverDict->getClasses().end();
        ++i)
    {
        const LandCoverClass* lcClass = i->get();
        int coverageValue = lcClass->getValue();
        if (coverageValue >= 0 && coverageValue < NUM_CLASSES)
        {
            CoverageClass& coverageClass = lut[coverageValue];
            const std::string& className = lcClass->getName();
            const SplatLUT::const_iterator k = _textureDef._splatLUT.find(className);
            if (k != _textureDef._splatLUT.end())
            {
                const SplatRangeDataVector& ranges = k->second;
                unsigned r = 0;
                for (unsigned lod = 0; lod < NUM_LODS; ++lod)
                {
                    const SplatRangeData& range = ranges[r];
                    write(coverageClass[lod], range);
                    if (range._maxLOD.isSet() && lod == range._maxLOD.get() && (r + 1) < ranges.size())
                        ++r;
                }
            }            
        }
    }

    // Encode the LUT into a texture buffer.
    osg::Image* image = new osg::Image();
    image->allocateImage(NUM_CLASSES * NUM_LODS, 1, 1, GL_RGBA32F_ARB, GL_FLOAT);

    // Populate the LUT image. Each LOD fits into a single RGBA GL_FLOAT vec4
    // by packing 6 floats into 4. See below for packing approach
    GLfloat* ptr = reinterpret_cast<GLfloat*>( image->data() );
    for (unsigned c=0; c<NUM_CLASSES; ++c)
    {
        for (unsigned lod=0; lod<NUM_LODS; ++lod)
        {
            LOD& record = lut[c][lod];

            *ptr++ = record.primary;
            *ptr++ = record.detail;

            // Pack two values into one float. First each value is truncated to a maximum
            // of 2 decimal places; then the first value goes left of the decimal, and the
            // second value goes to the right. The shader will unpack after reading.
            // We do this so that a single texelFetch call will retrieve the entire record.

            float b = (int)(record.brightness*100.0);
            float c = (int)(record.contrast*100.0);
            *ptr++ = b + (c/1000.0f);

            float t = (int)(record.threshold*100.0);
            float s = (int)(record.slope*100.0);
            *ptr++ = t + (s/1000.0f);
        }
    }

    // create a buffer object
    osg::TextureBuffer* buf = new osg::TextureBuffer();
    buf->setImage(image);
    buf->setInternalFormat(GL_RGBA32F_ARB);
    buf->setInternalFormatMode(osg::Texture::USE_IMAGE_DATA_FORMAT);
    buf->setUnRefImageDataAfterApply(true);

    // Tell the shader generator to skip the positioning texture.
    ShaderGenerator::setIgnoreHint(buf, true);

    return buf;
}
