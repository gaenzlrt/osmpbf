#include "osmblobfile.h"

#include <iostream>
#include <netinet/in.h>
#include <zlib.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "osmblob.pb.h"

namespace osmpbf {
	bool inflateData(const char * source, uint32_t sourceSize, char * dest, uint32_t destSize) {
		std::cout << "decompressing data ... ";
		int ret;
		z_stream stream;

		stream.zalloc = Z_NULL;
		stream.zfree = Z_NULL;
		stream.opaque = Z_NULL;
		stream.avail_in = sourceSize;
		stream.next_in = (Bytef *)source;
		stream.avail_out = destSize;
		stream.next_out = (Bytef *)dest;

		ret = inflateInit(&stream);
		if (ret != Z_OK)
			return false;

		ret = inflate(&stream, Z_NO_FLUSH);

		assert(ret != Z_STREAM_ERROR);

		switch (ret) {
		case Z_NEED_DICT:
			std::cerr << "ERROR: zlib - Z_NEED_DICT" << std::endl;
			inflateEnd(&stream);
			return false;
		case Z_DATA_ERROR:
			std::cerr << "ERROR: zlib - Z_DATA_ERROR" << std::endl;
			inflateEnd(&stream);
			return false;
		case Z_MEM_ERROR:
			std::cerr << "ERROR: zlib - Z_MEM_ERROR" << std::endl;
			inflateEnd(&stream);
			return false;
		default:
			break;
		}
		std::cout << "done" << std::endl;

		inflateEnd(&stream);
		return true;
	}

	bool deflateData(const char * source, uint32_t sourceSize, char * dest, uint32_t destSize) {
		return false;
	}

	AbstractBlobFile::AbstractBlobFile(std::string fileName) : m_FileName(fileName), m_FileDescriptor(-1) {
		GOOGLE_PROTOBUF_VERIFY_VERSION;
	}

	bool BlobFileIn::open() {
		close();

		std::cout << "opening File " << m_FileName << " ...";

		m_FileData = NULL;
		m_FileSize = 0;
		m_FilePos = 0;

		m_FileDescriptor = ::open(m_FileName.c_str(), O_RDONLY);
		if (m_FileDescriptor < 0) return false;

		struct stat stFileInfo;
		if (fstat(m_FileDescriptor, &stFileInfo) == 0) {
			if (stFileInfo.st_size > INT32_MAX) {
				std::cerr << "ERROR: input file is larger than 4GB" << std::endl;
				::close(m_FileDescriptor);
				m_FileDescriptor = -1;
				return false;
			}

			m_FileSize = uint32_t(stFileInfo.st_size);
		}

		m_FileData = (char *) mmap(0, m_FileSize, PROT_READ, MAP_SHARED, m_FileDescriptor, 0);

		if ((void *) m_FileData == MAP_FAILED) {
			std::cerr << "ERROR: could not mmap file" << std::endl;
			::close(m_FileDescriptor);
			m_FileDescriptor = -1;
			m_FileData = NULL;
			return false;
		}

		std::cout << "done" << std::endl;
		return true;
	}

	void BlobFileIn::close() {
		if (m_FileData) {
			std::cout << "closing file ...";
			munmap(m_FileData, m_FileSize);
			::close(m_FileDescriptor);
			std::cout << "done" << std::endl;

			m_FileData = NULL;
		}
	}

	BlobDataType BlobFileIn::readBlob(char * & buffer, uint32_t & bufferLength) {
		if (m_FilePos > m_FileSize - 1)
			return BLOB_Invalid;

		buffer = NULL;
		bufferLength = 0;

		std::cout << "== blob ==" << std::endl;
		BlobDataType blobDataType = BLOB_Invalid;

		std::cout << "checking blob header ..." << std::endl;

		uint32_t blobLength;
		uint32_t headerLength = ntohl(* (uint32_t *) fileData());

		std::cout << "header length : " << headerLength << " B" << std::endl;

		if (!headerLength)
			return BLOB_Invalid;

		m_FilePos += 4;

		std::cout << "parsing blob header ..." << std::endl;
		{
			BlobHeader * blobHeader = new BlobHeader();

			if (!blobHeader->ParseFromArray(fileData(), headerLength)) {
				std::cerr << "ERROR: invalid blob header structure" << std::endl;

				if (!blobHeader->has_type())
					std::cerr << "> no \"type\" field found" << std::endl;

				if (!blobHeader->has_datasize())
					std::cerr << "> no \"datasize\" field found" << std::endl;

				delete blobHeader;

				return BLOB_Invalid;
			}

			m_FilePos += headerLength;

			std::cout << "type : " << blobHeader->type() << std::endl;
			std::cout << "datasize : " << blobHeader->datasize() << " B ( " << blobHeader->datasize() / 1024.f << " KiB )" << std::endl;

			if (blobHeader->type() == "OSMHeader")
				blobDataType = BLOB_OSMHeader;
			else if (blobHeader->type() == "OSMData")
				blobDataType = BLOB_OSMData;

			blobLength = blobHeader->datasize();
			delete blobHeader;
		}

		if (blobDataType && blobLength) {
			std::cout << "parsing blob ..." << std::endl;

			Blob * blob = new Blob();

			if (!blob->ParseFromArray(fileData(), blobLength)) {
				std::cerr << "error: invalid blob structure" << std::endl;

				delete blob;

				return BLOB_Invalid;
			}

			m_FilePos += blobLength;

			if (blob->has_raw_size()) {
				std::cout << "found compressed blob data" << std::endl;
				std::cout << "uncompressed size : " << blob->raw_size() << "B ( " << blob->raw_size() / 1024.f << " KiB )" << std::endl;

				std::string * compressedData = blob->release_zlib_data();
				bufferLength = blob->raw_size();

				delete blob;

				buffer = new char[bufferLength];

				inflateData(compressedData->data(), compressedData->length(), buffer, bufferLength);
				delete compressedData;
			}
			else {
				std::cout << "found uncompressed blob data" << std::endl;

				std::string * uncompressedData = blob->release_raw();
				bufferLength = uncompressedData->length();

				delete blob;

				buffer = new char[bufferLength];

				memmove(buffer, uncompressedData->data(), bufferLength);
				delete uncompressedData;
			}

			return blobDataType;
		}

		std::cerr << "ERROR: ";
		if (!blobDataType)
			std::cerr << "invalid blob type";
		if (!bufferLength)
			std::cerr << "invalid blob size";
		std::cerr << std::endl;

		return BLOB_Invalid;
	}

	bool BlobFileOut::open() {
		m_FileDescriptor = ::open(m_FileName.c_str(), O_WRONLY | O_CREAT, 0666);
		return m_FileDescriptor > -1;
	}

	void BlobFileOut::close() {
		if (m_FileDescriptor > -1)
			::close(m_FileDescriptor);
	}

	void BlobFileOut::seek(uint32_t position) {
		::lseek(m_FileDescriptor, position, SEEK_SET);
	}

	uint32_t BlobFileOut::position() const {
		return ::lseek(m_FileDescriptor, 0, SEEK_CUR);
	}

	bool BlobFileOut::writeBlob(BlobDataType type, char * buffer, uint32_t bufferSize, bool compress) {
		if (type == BLOB_Invalid)
			return false;

		Blob * blob = new Blob();

		if (compress) {
			char * zlibBuffer = new char[bufferSize];
			uint32_t zlibBufferSize = bufferSize;
			deflateData(buffer, bufferSize, zlibBuffer, zlibBufferSize);
			blob->set_raw_size(bufferSize);
			blob->set_zlib_data((void *)zlibBuffer, zlibBufferSize);
			delete[] zlibBuffer;
		}
		else {
			blob->set_raw((void *)buffer, bufferSize);
		}

		if (!blob->IsInitialized()) {
			std::cerr << "blob not initialized" << std::endl;
			return false;
		}

		std::string serializedBlobBuffer = blob->SerializeAsString();
		delete blob;

// 		std::cout << "datasize: " << serializedBlobBuffer.length() << std::endl;
// 		return false;

		if (!serializedBlobBuffer.length()) {
			std::cerr << "serializedBlobBuffer failed" << std::endl;
			return false;
		}

		BlobHeader * blobHeader = new BlobHeader();
		blobHeader->set_datasize(serializedBlobBuffer.length());
		switch (type) {
		case BLOB_OSMData:
			blobHeader->set_type("OSMData"); break;
		case BLOB_OSMHeader:
			blobHeader->set_type("OSMHeader"); break;
		default:
			break;
		}

		if (!blobHeader->IsInitialized()) {
			std::cerr << "blobHeader not initialized" << std::endl;
			delete blobHeader;
			return false;
		}

		::lseek(m_FileDescriptor, sizeof(uint32_t), SEEK_CUR); // skip file size

		uint32_t headerPosition = ::lseek(m_FileDescriptor, 0, SEEK_CUR);
		if (!blobHeader->SerializeToFileDescriptor(m_FileDescriptor)) { // write header blob
			std::cerr << "error writing blobHeader" << std::endl;
			delete blobHeader;
			return false;
		}
		delete blobHeader;

		uint32_t blobPosition = ::lseek(m_FileDescriptor, 0, SEEK_CUR);
		uint32_t headerSize = blobPosition - headerPosition;

		headerSize = htonl(headerSize);

		::lseek(m_FileDescriptor, headerPosition - sizeof(uint32_t), SEEK_SET);
		::write(m_FileDescriptor, &headerSize, sizeof(uint32_t)); // write file size
		::lseek(m_FileDescriptor, blobPosition, SEEK_SET);
		::write(m_FileDescriptor, (void *) serializedBlobBuffer.data(), serializedBlobBuffer.length());

		return true;
	}
}
