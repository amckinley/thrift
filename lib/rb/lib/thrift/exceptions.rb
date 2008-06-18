module Thrift
  class Exception < StandardError
    def initialize(message)
      super
      @message = message
    end

    attr_reader :message
  end
  deprecate_class! :TException => Exception

  class ApplicationException < Exception

    UNKNOWN = 0
    UNKNOWN_METHOD = 1
    INVALID_MESSAGE_TYPE = 2
    WRONG_METHOD_NAME = 3
    BAD_SEQUENCE_ID = 4
    MISSING_RESULT = 5

    attr_reader :type

    def initialize(type=UNKNOWN, message=nil)
      super
      @type = type
    end

    def read(iprot)
      iprot.readStructBegin()
      while true
        fname, ftype, fid = iprot.readFieldBegin()
        if (ftype === Types::STOP)
          break
        end
        if (fid == 1)
          if (ftype === Types::STRING)
            @message = iprot.readString();
          else
            iprot.skip(ftype)
          end
        elsif (fid == 2)
          if (ftype === Types::I32)
            @type = iprot.readI32();
          else
            iprot.skip(ftype)
          end
        else
          iprot.skip(ftype)
        end
        iprot.readFieldEnd()
      end
      iprot.readStructEnd()
    end

    def write(oprot)
      oprot.writeStructBegin('Thrift::ApplicationException')
      if (@message != nil)
        oprot.writeFieldBegin('message', Types::STRING, 1)
        oprot.writeString(@message)
        oprot.writeFieldEnd()
      end
      if (@type != nil)
        oprot.writeFieldBegin('type', Types::I32, 2)
        oprot.writeI32(@type)
        oprot.writeFieldEnd()
      end
      oprot.writeFieldStop()
      oprot.writeStructEnd()
    end

  end
  deprecate_class! :TApplicationException => ApplicationException
end