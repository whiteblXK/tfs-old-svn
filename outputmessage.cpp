//////////////////////////////////////////////////////////////////////
// OpenTibia - an opensource roleplaying game
//////////////////////////////////////////////////////////////////////
//
//////////////////////////////////////////////////////////////////////
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software Foundation,
// Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//////////////////////////////////////////////////////////////////////
#include "otpch.h"

#include "outputmessage.h"
#include "connection.h"
#include "protocol.h"

OutputMessage::OutputMessage()
{
	freeMessage();
}

//*********** OutputMessagePool ****************//

OutputMessagePool::OutputMessagePool()
{
	OTSYS_THREAD_LOCKVARINIT(m_outputPoolLock);
	for(uint32_t i = 0; i < OUTPUT_POOL_SIZE; ++i)
	{
		OutputMessage* msg = new OutputMessage();
		m_outputMessages.push_back(msg);
#ifdef __TRACK_NETWORK__
		m_allOutputMessages.push_back(msg);
#endif
	}

	m_frameTime = OTSYS_TIME();
}

void OutputMessagePool::startExecutionFrame()
{
	m_frameTime = OTSYS_TIME();
	m_isOpen = true;
}

OutputMessagePool::~OutputMessagePool()
{
	OutputMessageVector::iterator it;
	for(it = m_outputMessages.begin(); it != m_outputMessages.end(); ++it)
		delete *it;

	m_outputMessages.clear();
	OTSYS_THREAD_LOCKVARRELEASE(m_outputPoolLock);
}

void OutputMessagePool::send(OutputMessage* msg)
{
	OTSYS_THREAD_LOCK(m_outputPoolLock, "");
	OutputMessage::OutputMessageState state = msg->getState();
	OTSYS_THREAD_UNLOCK(m_outputPoolLock, "");

	if(state == OutputMessage::STATE_ALLOCATED_NO_AUTOSEND)
	{
		#ifdef __DEBUG_NET_DETAIL__
		std::cout << "Sending message - SINGLE" << std::endl;
		#endif

		if(msg->getConnection())
		{
			if(msg->getConnection()->send(msg))
			{
				//Note: if we ever decide to change how the pool works this will have to change
				OTSYS_THREAD_LOCK(m_outputPoolLock, "");
				if(msg->getState() != OutputMessage::STATE_FREE)
					msg->setState(OutputMessage::STATE_WAITING);
				OTSYS_THREAD_UNLOCK(m_outputPoolLock, "");
			}
			else
			{
				msg->getProtocol()->onSendMessage(msg);
				internalReleaseMessage(msg);
			}
		}
		else
		{
			#ifdef __DEBUG_NET__
			std::cout << "Error: [OutputMessagePool::send] NULL connection." << std::endl;
			#endif
		}
	}
	else
	{
		#ifdef __DEBUG_NET__
		std::cout << "Warning: [OutputMessagePool::send] State != STATE_ALLOCATED_NO_AUTOSEND" << std::endl;
		#endif
	}
}

void OutputMessagePool::sendAll()
{
	OTSYS_THREAD_LOCK_CLASS lockClass(m_outputPoolLock);
	OutputMessageVector::iterator it;

	for(it = m_toAddQueue.begin(); it != m_toAddQueue.end();)
	{
		//drop messages that are older than 10 seconds
		if(OTSYS_TIME() - (*it)->getFrame() > 10000)
		{
			(*it)->getProtocol()->onSendMessage(*it);
			it = m_toAddQueue.erase(it);
			continue;
		}

		(*it)->setState(OutputMessage::STATE_ALLOCATED);
		m_autoSendOutputMessages.push_back(*it);
		++it;
	}
	m_toAddQueue.clear();

	for(it = m_autoSendOutputMessages.begin(); it != m_autoSendOutputMessages.end(); )
	{
		#ifdef __NO_PLAYER_SENDBUFFER__
		//use this define only for debugging
		bool v = 1;
		#else
		//It will send only messages bigger then 1 kb or with a lifetime greater than 10 ms
		bool v = (*it)->getMessageLength() > 1024 || (m_frameTime - (*it)->getFrame() > 10);
		#endif
		if(v)
		{
			#ifdef __DEBUG_NET_DETAIL__
			std::cout << "Sending message - ALL" << std::endl;
			#endif

			if((*it)->getConnection())
			{
				if((*it)->getConnection()->send(*it))
				{
					// Note: if we ever decide to change how the pool works this will have to change
					if((*it)->getState() != OutputMessage::STATE_FREE)
						(*it)->setState(OutputMessage::STATE_WAITING);
				}
				else
				{
					(*it)->getProtocol()->onSendMessage((*it));
					internalReleaseMessage(*it);
				}
			}
			else
			{
				#ifdef __DEBUG_NET__
				std::cout << "Error: [OutputMessagePool::send] NULL connection." << std::endl;
				#endif
			}
			m_autoSendOutputMessages.erase(it++);
		}
		else
			++it;
	}
}

void OutputMessagePool::internalReleaseMessage(OutputMessage* msg)
{
	if(msg->getProtocol())
	{
		msg->getProtocol()->unRef();
#ifdef __DEBUG_NET_DETAIL__
		std::cout << "Removing reference to protocol " << msg->getProtocol() << std::endl;
#endif
	}
	else
		std::cout << "No protocol found." << std::endl;

	if(msg->getConnection())
	{
		msg->getConnection()->unRef();
#ifdef __DEBUG_NET_DETAIL__
		std::cout << "Removing reference to connection " << msg->getConnection() << std::endl;
#endif
	}
	else
		std::cout << "No connection found." << std::endl;

	msg->freeMessage();
	m_outputMessages.push_back(msg);
}

void OutputMessagePool::releaseMessage(OutputMessage* msg, bool sent /*= false*/)
{
	OTSYS_THREAD_LOCK_CLASS lockClass(m_outputPoolLock);
	switch(msg->getState())
	{
		case OutputMessage::STATE_ALLOCATED:
		{
			OutputMessageVector::iterator it =
				std::find(m_autoSendOutputMessages.begin(), m_autoSendOutputMessages.end(), msg);
			if(it != m_autoSendOutputMessages.end())
				m_autoSendOutputMessages.erase(it);
			internalReleaseMessage(msg);
			break;
		}
		case OutputMessage::STATE_ALLOCATED_NO_AUTOSEND:
			internalReleaseMessage(msg);
			break;
		case OutputMessage::STATE_WAITING:
			if(!sent)
				std::cout << "Error: [OutputMessagePool::releaseMessage] Releasing STATE_WAITING OutputMessage." << std::endl;
			else
				internalReleaseMessage(msg);
			break;
		case OutputMessage::STATE_FREE:
			std::cout << "Error: [OutputMessagePool::releaseMessage] Releasing STATE_FREE OutputMessage." << std::endl;
			break;
		default:
			std::cout << "Error: [OutputMessagePool::releaseMessage] Releasing STATE_?(" << msg->getState() <<") OutputMessage." << std::endl;
			break;
	}
}

OutputMessage* OutputMessagePool::getOutputMessage(Protocol* protocol, bool autosend /*= true*/)
{
	#ifdef __DEBUG_NET_DETAIL__
	std::cout << "request output message - auto = " << autosend << std::endl;
	#endif

	if(!m_isOpen)
		return NULL;

	OTSYS_THREAD_LOCK_CLASS lockClass(m_outputPoolLock);

	if(protocol->getConnection() == NULL)
		return NULL;

	if(m_outputMessages.empty())
	{
		OutputMessage* msg = new OutputMessage();
		m_outputMessages.push_back(msg);
#ifdef __TRACK_NETWORK__
		m_allOutputMessages.push_back(msg);
#endif
	}

	OutputMessage* outputmessage;
	outputmessage = m_outputMessages.back();
	m_outputMessages.pop_back();

	configureOutputMessage(outputmessage, protocol, autosend);
	return outputmessage;
}

void OutputMessagePool::configureOutputMessage(OutputMessage* msg, Protocol* protocol, bool autosend)
{
	TRACK_MESSAGE(msg);

	msg->Reset();
	if(autosend)
	{
		msg->setState(OutputMessage::STATE_ALLOCATED);
		m_autoSendOutputMessages.push_back(msg);
	}
	else
		msg->setState(OutputMessage::STATE_ALLOCATED_NO_AUTOSEND);

	Connection* connection = protocol->getConnection();
	assert(connection != NULL);

	msg->setProtocol(protocol);
	protocol->addRef();
#ifdef __DEBUG_NET_DETAIL__
	std::cout << "Adding reference to protocol - " << protocol << std::endl;
#endif
	msg->setConnection(connection);
	connection->addRef();
#ifdef __DEBUG_NET_DETAIL__
	std::cout << "Adding reference to connection - " << connection << std::endl;
#endif
	msg->setFrame(m_frameTime);
}

void OutputMessagePool::addToAutoSend(OutputMessage* msg)
{
	m_toAddQueue.push_back(msg);
}
