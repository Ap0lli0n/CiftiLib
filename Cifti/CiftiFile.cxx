/*LICENSE_START*/ 
/*
 *  Copyright (c) 2014, Washington University School of Medicine
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without modification,
 *  are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *  this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 *  THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 *  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 *  EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "CiftiFile.h"

#include "CaretAssert.h"
#include "CiftiException.h"
#include "MultiDimArray.h"
#include "MultiDimIterator.h"
#include "NiftiIO.h"

#include <QFileInfo>

#include <iostream>

using namespace std;
using namespace boost;
using namespace cifti;

//private implementation classes
namespace cifti
{
    class CiftiOnDiskImpl : public CiftiFile::WriteImplInterface
    {
        mutable NiftiIO m_nifti;//because file objects aren't stateless (current position), so reading "changes" them
        CiftiXML m_xml;//because we need to parse it to set up the dimensions anyway
    public:
        CiftiOnDiskImpl(const QString& filename);//read-only
        CiftiOnDiskImpl(const QString& filename, const CiftiXML& xml, const CiftiVersion& version);//make new empty file with read/write
        void getRow(float* dataOut, const std::vector<int64_t>& indexSelect, const bool& tolerateShortRead) const;
        void getColumn(float* dataOut, const int64_t& index) const;
        const CiftiXML& getCiftiXML() const { return m_xml; }
        QString getFilename() const { return m_nifti.getFilename(); }
        void setRow(const float* dataIn, const std::vector<int64_t>& indexSelect);
        void setColumn(const float* dataIn, const int64_t& index);
    };
    
    class CiftiMemoryImpl : public CiftiFile::WriteImplInterface
    {
        MultiDimArray<float> m_array;
    public:
        CiftiMemoryImpl(const CiftiXML& xml);
        void getRow(float* dataOut, const std::vector<int64_t>& indexSelect, const bool& tolerateShortRead) const;
        void getColumn(float* dataOut, const int64_t& index) const;
        bool isInMemory() const { return true; }
        void setRow(const float* dataIn, const std::vector<int64_t>& indexSelect);
        void setColumn(const float* dataIn, const int64_t& index);
    };
}

CiftiFile::ReadImplInterface::~ReadImplInterface()
{
}

CiftiFile::WriteImplInterface::~WriteImplInterface()
{
}

CiftiFile::CiftiFile(const QString& fileName)
{
    openFile(fileName);
}

void CiftiFile::openFile(const QString& fileName)
{
    m_writingImpl.reset();
    m_readingImpl.reset();//to make sure it closes everything first, even if the open throws
    m_dims.clear();
    shared_ptr<CiftiOnDiskImpl> newRead(new CiftiOnDiskImpl(QFileInfo(fileName).absoluteFilePath()));//this constructor opens existing file read-only
    m_readingImpl = newRead;//it should be noted that if the constructor throws (if the file isn't readable), new guarantees the memory allocated for the object will be freed
    m_xml = newRead->getCiftiXML();
    m_dims = m_xml.getDimensions();
}

void CiftiFile::setWritingFile(const QString& fileName)
{
    m_writingFile = QFileInfo(fileName).absoluteFilePath();//always resolve paths as soon as they enter CiftiFile, in case some clown changes directory before writing data
    m_writingImpl.reset();//prevent writing to previous writing implementation, let the next set...() set up for writing
}

void CiftiFile::writeFile(const QString& fileName, const CiftiVersion& writingVersion)
{
    if (m_readingImpl == NULL || m_dims.empty()) throw CiftiException("writeFile called on uninitialized CiftiFile");
    QFileInfo myInfo(fileName);
    QString canonicalFilename = myInfo.canonicalFilePath();//NOTE: returns EMPTY STRING for nonexistant file
    const CiftiOnDiskImpl* testImpl = dynamic_cast<CiftiOnDiskImpl*>(m_readingImpl.get());
    bool collision = false;
    if (testImpl != NULL && canonicalFilename != "" && QFileInfo(testImpl->getFilename()).canonicalFilePath() == canonicalFilename)
    {
        collision = true;//empty string test is so that we don't say collision if both are nonexistant - could happen if file is removed/unlinked while reading on some filesystems
    }
    if (collision)
    {
        if (m_writingVersion == writingVersion) return;//don't need to copy to itself
        convertToInMemory();//otherwise, we need to preserve the contents first - if writing fails, we will end up with it converted to in-memory, but oh well
    }
    shared_ptr<WriteImplInterface> tempWrite(new CiftiOnDiskImpl(myInfo.absoluteFilePath(), m_xml, writingVersion));
    vector<int64_t> iterateDims(m_dims.begin() + 1, m_dims.end());//above constructor creates new file in read/write mode
    vector<float> scratchRow(m_dims[0]);
    for (MultiDimIterator<int64_t> iter(iterateDims); !iter.atEnd(); ++iter)
    {
        m_readingImpl->getRow(scratchRow.data(), *iter, false);
        tempWrite->setRow(scratchRow.data(), *iter);
    }
    if (collision)//drop the in-memory representation afterwards
    {
        m_writingVersion = writingVersion;//also record the current version number
        m_writingImpl = tempWrite;
        m_readingImpl = tempWrite;
    }
}

void CiftiFile::writeFile(const QString& fileName)
{
    writeFile(fileName, m_writingVersion);//let the more complex case handle the simple one too, will always return early on collision
}

void CiftiFile::convertToInMemory()
{
    if (isInMemory()) return;
    if (m_readingImpl == NULL  || m_dims.empty())//not set up yet
    {
        m_writingFile = "";//make sure it doesn't do on-disk when set...() is called
        return;
    }
    shared_ptr<WriteImplInterface> tempWrite(new CiftiMemoryImpl(m_xml));//if we get an error while reading, free the memory immediately
    vector<int64_t> iterateDims(m_dims.begin() + 1, m_dims.end());
    vector<float> scratchRow(m_dims[0]);
    for (MultiDimIterator<int64_t> iter(iterateDims); !iter.atEnd(); ++iter)
    {
        m_readingImpl->getRow(scratchRow.data(), *iter, false);
        tempWrite->setRow(scratchRow.data(), *iter);
    }
    m_writingImpl = tempWrite;
    m_readingImpl = tempWrite;
}

bool CiftiFile::isInMemory() const
{
    if (m_readingImpl == NULL)
    {
        return (m_writingFile == "");//return what it would be if verifyWriteImpl() was called
    } else {
        return m_readingImpl->isInMemory();
    }
}

void CiftiFile::getRow(float* dataOut, const vector<int64_t>& indexSelect, const bool& tolerateShortRead) const
{
    if (m_dims.empty()) throw CiftiException("getRow called on uninitialized CiftiFile");
    if (m_readingImpl == NULL) return;//NOT an error because we are pretending to have a matrix already, while we are waiting for setRow to actually start writing the file
    m_readingImpl->getRow(dataOut, indexSelect, tolerateShortRead);
}

void CiftiFile::getColumn(float* dataOut, const int64_t& index) const
{
    if (m_dims.empty()) throw CiftiException("getColumn called on uninitialized CiftiFile");
    if (m_dims.size() != 2) throw CiftiException("getColumn called on non-2D CiftiFile");
    if (m_readingImpl == NULL) return;//NOT an error because we are pretending to have a matrix already, while we are waiting for setRow to actually start writing the file
    m_readingImpl->getColumn(dataOut, index);
}

void CiftiFile::setCiftiXML(const CiftiXML& xml, const bool useOldMetadata, const CiftiVersion& writingVersion)
{
    if (xml.getNumberOfDimensions() == 0) throw CiftiException("setCiftiXML called with 0-dimensional CiftiXML");
    m_writingVersion = writingVersion;
    if (useOldMetadata)
    {
        MetaData newmd = m_xml.getFileMetaData();//make a copy
        m_xml = xml;//because this will overwrite the metadata
        m_xml.setFileMetaData(newmd);
    } else {
        m_xml = xml;
    }
    m_dims = m_xml.getDimensions();
    m_readingImpl.reset();//drop old implementation, as it is now invalid due to XML (and therefore matrix size) change
    m_writingImpl.reset();
}

void CiftiFile::setRow(const float* dataIn, const vector<int64_t>& indexSelect)
{
    verifyWriteImpl();
    m_writingImpl->setRow(dataIn, indexSelect);
}

void CiftiFile::setColumn(const float* dataIn, const int64_t& index)
{
    verifyWriteImpl();
    if (m_dims.size() != 2) throw CiftiException("setColumn called on non-2D CiftiFile");
    m_writingImpl->setColumn(dataIn, index);
}

//single-index functions
void CiftiFile::getRow(float* dataOut, const int64_t& index, const bool& tolerateShortRead) const
{
    if (m_dims.empty()) throw CiftiException("getRow called on uninitialized CiftiFile");
    if (m_dims.size() != 2) throw CiftiException("getRow with single index called on non-2D CiftiFile");
    if (m_readingImpl == NULL) return;//NOT an error because we are pretending to have a matrix already, while we are waiting for setRow to actually start writing the file
    vector<int64_t> tempvec(1, index);//could use a member if we need more speed
    m_readingImpl->getRow(dataOut, tempvec, tolerateShortRead);
}

void CiftiFile::getRow(float* dataOut, const int64_t& index) const
{
    getRow(dataOut, index, false);//once CiftiInterface is gone, we can collapse this into a default value
}

void CiftiFile::setRow(const float* dataIn, const int64_t& index)
{
    verifyWriteImpl();
    if (m_dims.size() != 2) throw CiftiException("setRow with single index called on non-2D CiftiFile");
    vector<int64_t> tempvec(1, index);//could use a member if we need more speed
    m_writingImpl->setRow(dataIn, tempvec);
}
//*///end single-index functions

void CiftiFile::verifyWriteImpl()
{//this is where the magic happens - we want to emulate being a simple in-memory file, but actually be reading/writing on-disk when possible
    if (m_writingImpl != NULL) return;
    CaretAssert(!m_dims.empty());//if the xml hasn't been set, then we can't do anything meaningful
    if (m_dims.empty()) throw CiftiException("setRow or setColumn attempted on uninitialized CiftiFile");
    if (m_writingFile == "")
    {
        if (m_readingImpl != NULL)
        {
            convertToInMemory();
        } else {
            m_writingImpl = shared_ptr<CiftiMemoryImpl>(new CiftiMemoryImpl(m_xml));
        }
    } else {
        if (m_readingImpl != NULL)
        {
            CiftiOnDiskImpl* testImpl = dynamic_cast<CiftiOnDiskImpl*>(m_readingImpl.get());
            if (testImpl != NULL)
            {
                QString canonicalCurrent = QFileInfo(testImpl->getFilename()).canonicalFilePath();//returns "" if nonexistant, if unlinked while open
                if (canonicalCurrent != "" && canonicalCurrent == QFileInfo(m_writingFile).canonicalFilePath())//these were already absolute
                {
                    convertToInMemory();//save existing data in memory before we clobber file
                }
            }
        }
        m_writingImpl = shared_ptr<CiftiOnDiskImpl>(new CiftiOnDiskImpl(m_writingFile, m_xml, m_writingVersion));//this constructor makes new file for writing
        if (m_readingImpl != NULL)
        {
            vector<int64_t> iterateDims(m_dims.begin() + 1, m_dims.end());
            vector<float> scratchRow(m_dims[0]);
            for (MultiDimIterator<int64_t> iter(iterateDims); !iter.atEnd(); ++iter)
            {
                m_readingImpl->getRow(scratchRow.data(), *iter, false);
                m_writingImpl->setRow(scratchRow.data(), *iter);
            }
        }
    }
    m_readingImpl = m_writingImpl;//read-only implementations are set up in specialized functions
}

CiftiMemoryImpl::CiftiMemoryImpl(const CiftiXML& xml)
{
    CaretAssert(xml.getNumberOfDimensions() != 0);
    m_array.resize(xml.getDimensions());
}

void CiftiMemoryImpl::getRow(float* dataOut, const vector<int64_t>& indexSelect, const bool&) const
{
    const float* ref = m_array.get(1, indexSelect);
    int64_t rowSize = m_array.getDimensions()[0];//we don't accept 0-D CiftiXML, so this will always work
    for (int64_t i = 0; i < rowSize; ++i)
    {
        dataOut[i] = ref[i];
    }
}

void CiftiMemoryImpl::getColumn(float* dataOut, const int64_t& index) const
{
    CaretAssert(m_array.getDimensions().size() == 2);//otherwise, CiftiFile shouldn't have called this
    const float* ref = m_array.get(2, vector<int64_t>());//empty vector is intentional, only 2 dimensions exist, so no more to select from
    int64_t rowSize = m_array.getDimensions()[0];
    int64_t colSize = m_array.getDimensions()[1];
    CaretAssert(index >= 0 && index < rowSize);//because we are doing the indexing math manually for speed
    for (int64_t i = 0; i < colSize; ++i)
    {
        dataOut[i] = ref[index + rowSize * i];
    }
}

void CiftiMemoryImpl::setRow(const float* dataIn, const vector<int64_t>& indexSelect)
{
    float* ref = m_array.get(1, indexSelect);
    int64_t rowSize = m_array.getDimensions()[0];//we don't accept 0-D CiftiXML, so this will always work
    for (int64_t i = 0; i < rowSize; ++i)
    {
        ref[i] = dataIn[i];
    }
}

void CiftiMemoryImpl::setColumn(const float* dataIn, const int64_t& index)
{
    CaretAssert(m_array.getDimensions().size() == 2);//otherwise, CiftiFile shouldn't have called this
    float* ref = m_array.get(2, vector<int64_t>());//empty vector is intentional, only 2 dimensions exist, so no more to select from
    int64_t rowSize = m_array.getDimensions()[0];
    int64_t colSize = m_array.getDimensions()[1];
    CaretAssert(index >= 0 && index < rowSize);//because we are doing the indexing math manually for speed
    for (int64_t i = 0; i < colSize; ++i)
    {
        ref[index + rowSize * i] = dataIn[i];
    }
}

CiftiOnDiskImpl::CiftiOnDiskImpl(const QString& filename)
{//opens existing file for reading
    m_nifti.openRead(filename);//read-only, so we don't need write permission to read a cifti file
    const NiftiHeader& myHeader = m_nifti.getHeader();
    int numExts = (int)myHeader.m_extensions.size(), whichExt = -1;
    for (int i = 0; i < numExts; ++i)
    {
        if (myHeader.m_extensions[i]->m_ecode == NIFTI_ECODE_CIFTI)
        {
            whichExt = i;
            break;
        }
    }
    if (whichExt == -1) throw CiftiException("no cifti extension found in file '" + filename + "'");
    m_xml.readXML(QByteArray(myHeader.m_extensions[whichExt]->m_bytes.data(), myHeader.m_extensions[whichExt]->m_bytes.size()));//CiftiXML should be under 2GB
    vector<int64_t> dimCheck = m_nifti.getDimensions();
    if (dimCheck.size() < 5) throw CiftiException("invalid dimensions in cifti file '" + filename + "'");
    for (int i = 0; i < 4; ++i)
    {
        if (dimCheck[i] != 1) throw CiftiException("non-singular dimension #" + QString::number(i + 1) + " in cifti file '" + filename + "'");
    }
    if (m_xml.getParsedVersion().hasReversedFirstDims())
    {
        while (dimCheck.size() < 6) dimCheck.push_back(1);//just in case
        int64_t temp = dimCheck[4];//note: nifti dim[5] is the 5th dimension, index 4 in this vector
        dimCheck[4] = dimCheck[5];
        dimCheck[5] = temp;
        m_nifti.overrideDimensions(dimCheck);
    }
    if (m_xml.getNumberOfDimensions() + 4 != (int)dimCheck.size()) throw CiftiException("XML does not match number of nifti dimensions in file " + filename + "'");
    for (int i = 4; i < (int)dimCheck.size(); ++i)
    {
        if (m_xml.getDimensionLength(i - 4) < 1)//CiftiXML will only let this happen with cifti-1
        {
            m_xml.getSeriesMap(i - 4).setLength(dimCheck[i]);//and only in a series map
        } else {
            if (m_xml.getDimensionLength(i - 4) != dimCheck[i])
            {
                throw CiftiException("xml and nifti header disagree on matrix dimensions");
            }
        }
    }
}

CiftiOnDiskImpl::CiftiOnDiskImpl(const QString& filename, const CiftiXML& xml, const CiftiVersion& version)
{//starts writing new file
    NiftiHeader outHeader;
    outHeader.setDataType(NIFTI_TYPE_FLOAT32);//actually redundant currently, default is float32
    char intentName[16];
    int32_t intentCode = xml.getIntentInfo(version, intentName);
    outHeader.setIntent(intentCode, intentName);
    QByteArray xmlBytes = xml.writeXMLToQByteArray(version);
    shared_ptr<NiftiExtension> outExtension(new NiftiExtension());
    outExtension->m_ecode = NIFTI_ECODE_CIFTI;
    int numBytes = xmlBytes.size();
    outExtension->m_bytes.resize(numBytes);
    for (int i = 0; i < numBytes; ++i)
    {
        outExtension->m_bytes[i] = xmlBytes[i];
    }
    outHeader.m_extensions.push_back(outExtension);
    vector<int64_t> matrixDims = xml.getDimensions();
    vector<int64_t> niftiDims(4, 1);//the reserved space and time dims
    niftiDims.insert(niftiDims.end(), matrixDims.begin(), matrixDims.end());
    if (version.hasReversedFirstDims())
    {
        vector<int64_t> headerDims = niftiDims;
        while (headerDims.size() < 6) headerDims.push_back(1);//just in case
        int64_t temp = headerDims[4];
        headerDims[4] = headerDims[5];
        headerDims[5] = temp;
        outHeader.setDimensions(headerDims);//give the header the reversed dimensions
        m_nifti.writeNew(filename, outHeader, 2, true);
        m_nifti.overrideDimensions(niftiDims);//and then tell the nifti reader to use the correct dimensions
    } else {
        outHeader.setDimensions(niftiDims);
        m_nifti.writeNew(filename, outHeader, 2, true);
    }
    m_xml = xml;
}

void CiftiOnDiskImpl::getRow(float* dataOut, const vector<int64_t>& indexSelect, const bool& tolerateShortRead) const
{
    m_nifti.readData(dataOut, 5, indexSelect, tolerateShortRead);//5 means 4 reserved (space and time) plus the first cifti dimension
}

void CiftiOnDiskImpl::getColumn(float* dataOut, const int64_t& index) const
{
    CaretAssert(m_xml.getNumberOfDimensions() == 2);//otherwise this shouldn't be called
    CaretAssert(index >= 0 && index < m_xml.getDimensionLength(CiftiXML::ALONG_ROW));
    vector<int64_t> indexSelect(2);
    indexSelect[0] = index;
    int64_t colLength = m_xml.getDimensionLength(CiftiXML::ALONG_COLUMN);
    for (int64_t i = 0; i < colLength; ++i)//assume if they really want getColumn on disk, they don't want their pagecache obliterated, so read it 1 element at a time
    {
        indexSelect[1] = i;
        m_nifti.readData(dataOut + i, 4, indexSelect);//4 means just the 4 reserved dimensions, so 1 element of the matrix
    }
}

void CiftiOnDiskImpl::setRow(const float* dataIn, const vector<int64_t>& indexSelect)
{
    m_nifti.writeData(dataIn, 5, indexSelect);
}

void CiftiOnDiskImpl::setColumn(const float* dataIn, const int64_t& index)
{
    CaretAssert(m_xml.getNumberOfDimensions() == 2);//otherwise this shouldn't be called
    CaretAssert(index >= 0 && index < m_xml.getDimensionLength(CiftiXML::ALONG_ROW));
    vector<int64_t> indexSelect(2);
    indexSelect[0] = index;
    int64_t colLength = m_xml.getDimensionLength(CiftiXML::ALONG_COLUMN);
    for (int64_t i = 0; i < colLength; ++i)//don't do RMW, so write it 1 element at a time
    {
        indexSelect[1] = i;
        m_nifti.writeData(dataIn + i, 4, indexSelect);//4 means just the 4 reserved dimensions, so 1 element of the matrix
    }
}
