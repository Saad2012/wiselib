
#include "external_interface/external_interface.h"
#include "external_interface/external_interface_testing.h"
using namespace wiselib;
typedef OSMODEL Os;
typedef Os::block_data_t block_data_t;
typedef Os::size_t size_type;
typedef Os::Uart Uart;

	// Enable dynamic memory allocation using malloc() & free()
	#include "util/allocators/malloc_free_allocator.h"
	typedef MallocFreeAllocator<Os> Allocator;
	Allocator& get_allocator();

#include <util/split_n3.h>
#include <util/serialization/serialization.h>
#include <util/meta.h>
#include <algorithms/hash/crc16.h>
#include <algorithms/codecs/base64_codec.h>

#include <iostream>
#include <time.h>

class App {
	// {{{
	public:
		Os::Debug::self_pointer_t debug_;
		Os::Timer::self_pointer_t timer_;
		block_data_t rdf_[1024];
		size_t bytes_sent_;
		size_t triples;

		enum { TUPLE_SLEEP = 50 };
		enum { SEND_INTERVAL = 30000 };


		::uint32_t wait; // * 10s

		void init(Os::AppMainParameter& amp) {
			debug_ = &wiselib::FacetProvider<Os, Os::Debug>::get_facet(amp);
			timer_ = &wiselib::FacetProvider<Os, Os::Timer>::get_facet(amp);

			bytes_sent_ = 0;
			
			wait = 1;
			timer_->set_timer<App, &App::go>(10000, this, 0);
		}

		void go(void*) {
			wait--;
			if(wait == 0) {
				init_serial();
				read_boot_babble();
				main_loop();
			}
			else {
				timer_->set_timer<App, &App::go>(10000, this, 0);
			}
		}




		fd_set fds_serial, fds_none;
		int serial_fd;

		void init_serial() {
			struct termios options;
			speed_t speed = B115200;
			//speed_t speed = B57600;
			//char *device = "/dev/ttyUSB0";
			char *device = "/dev/tmotesky1";
			//char *timeformat = NULL;
			//int nfound, flags = 0;
			//unsigned char lastc = '\0';
			
			//serial_fd = ::open(device, O_RDWR | O_NOCTTY | O_NDELAY | O_DIRECT | O_SYNC);
//			serial_fd = ::open(device, O_RDWR | O_NOCTTY | O_NDELAY | O_SYNC);
//#ifndef __APPLE__
  //serial_fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY | O_DIRECT | O_SYNC );
//#else
  serial_fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY | O_SYNC );
//#endif

			if(serial_fd < 0) {
				perror(device);
				exit(-1);
			}

			debug_->debug("opening %s...", device);

			if(fcntl(serial_fd, F_SETFL, 0) < 0) {
				perror("fcntl problem");
				exit(-1);
			}
			if(tcgetattr(serial_fd, &options) < 0) {
				perror("couldnt get options");
				exit(-1);
			}

			cfsetispeed(&options, speed);
			cfsetospeed(&options, speed);
			options.c_cflag |= CLOCAL | CREAD;
			options.c_cflag &= ~(CSIZE | PARENB | PARODD);
			options.c_cflag |= CS8;

			options.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
			options.c_oflag &= ~OPOST;

			if(tcsetattr(serial_fd, TCSANOW, &options) < 0) {
				perror("couldnt set options");
				exit(-1);
			}

			FD_ZERO(&fds_serial);
			FD_SET(serial_fd, &fds_serial);
		}

		void send_serial(size_t n, block_data_t* data) {
			for(size_t i = 0; i < n; i++) {
				if(::write(serial_fd, data + i, 1) <= 0) {
					perror("write");
					exit(1);
				}
				else {
					fflush(NULL);
					usleep(6000);
				}
			}
		}


		void read_boot_babble() {
			enum { DURATION = 20 };

			debug_->debug("waiting for node to boot (%ds)", (int)DURATION);
			
			::uint32_t t = time(0);

			while(time(0) - t < DURATION) {
				struct timeval timeout;
				timeout.tv_sec = 1;
				timeout.tv_usec = 0;
				FD_ZERO(&fds_serial);
				FD_SET(serial_fd, &fds_serial);
				int nfound = select(FD_SETSIZE, &fds_serial, (fd_set*)0, (fd_set*)0, &timeout);
				if(nfound > 0) {
					block_data_t buf[1024];
					int n = ::read(serial_fd, buf, sizeof(buf));
					if(n >= 0 && n <= 1024) {
						buf[n] = 0;
						debug_->debug("[%3d] %s", (int)n, (char*)buf);
					}
					else {
						debug_->debug("[%3d]", (int)n);
					}
				}
			}

		}


		bool expect_answer;

		void main_loop() {
			char line[1024];
			SplitN3<Os> n3;

			bytes_sent_ = 0;
			bool need_line = true;
			while(std::cin) {
				// read line from stdin
				if(need_line) {
					std::cin.getline(line, sizeof(line));
					n3.parse_line(line);
					need_line = false;
				}

				size_t l0 = strlen(n3[0]),
					   l1 = strlen(n3[1]),
					   l2 = strlen(n3[2]);
				size_t lensum = l0 + l1 + l2 + 3;

				expect_answer = false;

				if(bytes_sent_ + lensum > 1020) {
					// send summary (=footer)
					::uint16_t checksum = Crc16<Os>::hash(rdf_, bytes_sent_);
					block_data_t summary[] = {0, 0, 0, 0};
					wiselib::write<Os, block_data_t, ::uint16_t>(summary, checksum);
					send_serial(4, summary);
					debug_->debug("sent %d tuples chk=%x b=%d", (int)triples, (unsigned)checksum, (int)bytes_sent_);
					expect_answer = true;
				}
				else {
					for(size_type i = 0; i < 3; i++) {
						strcpy((char*)rdf_ + bytes_sent_, (char*)n3[i]);
						send_serial(strlen(n3[i]) + 1, reinterpret_cast<block_data_t*>(n3[i]));
						bytes_sent_ += strlen(n3[i]) + 1;
					}
					triples++;
					need_line = true;
				}

				while(expect_answer) {
					struct timeval timeout;
					timeout.tv_sec = 10;
					timeout.tv_usec = 0;
					FD_ZERO(&fds_serial);
					FD_SET(serial_fd, &fds_serial);
					debug_->debug("waiting for answer...");
					int nfound = select(FD_SETSIZE, &fds_serial, (fd_set*)0, (fd_set*)0, &timeout);
					debug_->debug("select() returned %d", (int)nfound);
					send_n(nfound);
				}


			} // while cin

				if(bytes_sent_ > 0) {
					// send summary (=footer)
					::uint16_t checksum = Crc16<Os>::hash(rdf_, bytes_sent_);
					block_data_t summary[] = {0, 0, 0, 0};
					wiselib::write<Os, block_data_t, ::uint16_t>(summary, checksum);
					send_serial(4, summary);
					debug_->debug("final sent %d tuples chk=%x b=%d", (int)triples, (unsigned)checksum, (int)bytes_sent_);
					expect_answer = true;
					//timer_->set_timer<App, &App::on_uart_timeout>(UART_RESEND_
					while(expect_answer) {
						struct timeval timeout;
						timeout.tv_sec = 10;
						timeout.tv_usec = 0;
						FD_ZERO(&fds_serial);
						FD_SET(serial_fd, &fds_serial);
						debug_->debug("waiting for answer...");
						int nfound = select(FD_SETSIZE, &fds_serial, (fd_set*)0, (fd_set*)0, &timeout);
						debug_->debug("select() returned %d", (int)nfound);
						send_n(nfound);
					}
				}

		}

		/**
		 * Read answer of length nfound from uart, and take the appropriate
		 * measure (abort, resend, do nothing).
		 */
		void send_n(int nfound) {
			if(nfound < 0) {
				if(errno == EINTR) {
					fprintf(stderr, "syscal interrupted\n");
					return;
				}
				perror("select fuckup");
				exit(1);
			}
			else if(nfound == 0) {
				// Send everything again!

				enum { BUFSIZE = 100 };
				debug_->debug("resending b=%d", (int)bytes_sent_);

				// first send an end-marker with a wrong CRC so the position
				// counter in the gateway gets reset
				block_data_t reset[] = { 0, 0, 0, 0 };
				//uart_->write(4, reset);
				debug_->debug("sending reset");
				send_serial(4, reset);
				timer_->sleep(3*TUPLE_SLEEP);

				size_type snt = 0;
				while(snt < bytes_sent_) {
					size_type s = ((bytes_sent_ - snt) < BUFSIZE) ? (bytes_sent_ - snt) : BUFSIZE;
					debug_->debug("resending chunk @%d", (int)snt);
					send_serial(s, (block_data_t*)rdf_ + snt);
					timer_->sleep(TUPLE_SLEEP);
					snt += s;
				}
				debug_->debug("resend done");
				//timer_->set_timer<App, &App::on_uart_timeout>(UART_RESEND_INTERVAL, this, (void*)ato_guard_);
			}
			else {
				block_data_t buf[100];

				if(FD_ISSET(serial_fd, &fds_serial)) {
					int n = ::read(serial_fd, buf, sizeof(buf));
					if(n < 0) {
						perror("couldnt read");
						exit(-1);
					}

					if(n >= 1 && buf[0] == 'O') { // && buf[1] == 'K') {
						debug_->debug("OK");
						expect_answer = false;
						bytes_sent_ = 0;
						timer_->sleep(SEND_INTERVAL);
					}
					else { debug_->debug("{{%s}}", (char*)buf); }
				}
			}
		}









#if 0
		Os::Uart::self_pointer_t uart_;
		Os::Debug::self_pointer_t debug_;
		Os::Timer::self_pointer_t timer_;
		block_data_t rdf_buffer_[1024];
		size_type bytes_received_;
		size_type bytes_sent_;

		enum {
			SEND_INTERVAL_START = 30000,
			SEND_INTERVAL = 30000,
			TUPLE_SLEEP = 100,
			UART_RESEND_INTERVAL = 10000,
		};

		void init(Os::AppMainParameter& amp) {
      		uart_ = &wiselib::FacetProvider<Os, Uart>::get_facet(amp);
      		debug_ = &wiselib::FacetProvider<Os, Os::Debug>::get_facet(amp);
      		timer_ = &wiselib::FacetProvider<Os, Os::Timer>::get_facet(amp);
			uart_->set_address("/dev/tmotesky1");
			//uart_->set_address("/dev/ttyUSB0");
			uart_->enable_serial_comm();
			uart_->reg_read_callback<App, &App::on_receive_uart>(this);

			bytes_received_ = 0;
			bytes_sent_ = 0;
			buf_empty = true;

			waiting_for_resend = false;
			timer_->set_timer<App, &App::read_n3>(SEND_INTERVAL_START, this, 0);
		}
		bool waiting_for_resend;

		char buf[1024];
		bool buf_empty;

		// Most of the serial code is stolen from serialdump

		int serial_fd;












		void send_contiki(size_type l, block_data_t *data) {
			//block_data_t buffer[1024];
			//Base64Codec<Os>::encode(data, buffer, l);
			//buffer[2 * l] = '\n';
			//buffer[2 * l + 1] = 0;
			//debug_->debug("-> %s", reinterpret_cast<char*>(buffer));
			//uart_->write(2 * l, reinterpret_cast<Uart::block_data_t* >(buffer));
			uart_->write(l, reinterpret_cast<Uart::block_data_t* >(data));
		}



		void read_n3(void *) {
			typedef SplitN3<Os> Splitter;
			Splitter sp;

			size_type triples = 0;
			bytes_sent_ = 0;

			debug_->debug("starting uart send");

			while(std::cin) {

				if(buf_empty) {
					std::cin.getline(buf, 1024);
					buf_empty = false;
				}

				if(buf[0] == 0) { break; }
				//debug_->debug("PARSE: [[[%s]]]", (char*)buf);

				sp.parse_line(buf);

				size_t lensum = strlen(sp[0]) + strlen(sp[1]) + strlen(sp[2]) + 3;

				if(bytes_sent_ + lensum > 1020) {
					// this tuple wouldnt fit, send summary and be done with
					// it!
					::uint16_t checksum = Crc16<Os>::hash(rdf_buffer_, bytes_sent_);
					block_data_t summary[] = {0, 0, 0, 0};
					wiselib::write<Os, block_data_t, ::uint16_t>(summary, checksum);
					/*
					uart_->write(4, reinterpret_cast<Uart::block_data_t*>(summary));
					*/
					debug_->debug("sending summary");
					send_contiki(4, summary);
					//timer_->sleep(TUPLE_SLEEP);
					debug_->debug("sent %d tuples chk=%x b=%d", (int)triples, (unsigned)checksum, (int)bytes_sent_);
					timer_->set_timer<App, &App::on_uart_timeout>(UART_RESEND_INTERVAL, this, (void*)ato_guard_);
					triples = 0;

					return;
				}
				else {
					for(size_type i = 0; i < 3; i++) {
						debug_->debug("i=%d", (int)i);
						strcpy((char*)rdf_buffer_ + bytes_sent_, (char*)sp[i]);
						debug_->debug("WR UART: (%d) %s", (int)strlen(sp[i]) + 1, (char*)sp[i]);
						//uart_->write(strlen(sp[i]) + 1, sp[i]);
						send_contiki(strlen(sp[i]) + 1, reinterpret_cast<block_data_t*>(sp[i]));
						timer_->sleep(TUPLE_SLEEP);
						bytes_sent_ += strlen(sp[i]) + 1;
					}
					buf_empty = true;
					triples++;
				}
			} // while cin

			if(bytes_sent_) {

				::uint16_t checksum = Crc16<Os>::hash(rdf_buffer_, bytes_sent_);
				block_data_t summary[] = {0, 0, 0, 0};
				wiselib::write<Os, block_data_t, ::uint16_t>(summary, checksum);
				//uart_->write(4, reinterpret_cast<Uart::block_data_t*>(summary));
				debug_->debug("sending final summary");
				send_contiki(4, summary);
				timer_->sleep(TUPLE_SLEEP);
				debug_->debug("final sent %d tuples chk=%x b=%d", (int)triples, (unsigned)checksum, (int)bytes_sent_);
				buf_empty = true;
				triples = 0;
			}


		} // read_n3

		Uvoid ato_guard_;
		void on_uart_timeout(void*ato_guard) {
			debug_->debug("uart timeout %lu,%lu", (unsigned long)(Uvoid)ato_guard, (unsigned long)ato_guard_);
			if((Uvoid)ato_guard != ato_guard_) { return; }

			uart_resend();
		}

		void uart_resend(void* _=0) {
				enum { BUFSIZE = 100 };
			waiting_for_resend = false;

				debug_->debug("resending b=%d", (int)bytes_sent_);

			// first send an end-marker with a wrong CRC so the position
			// counter in the gateway gets reset
			block_data_t reset[] = { 0, 0, 0, 0 };
			//uart_->write(4, reset);
			debug_->debug("sending reset");
			send_contiki(4, reset);
			timer_->sleep(3*TUPLE_SLEEP);

			size_type snt = 0;
			while(snt < bytes_sent_) {
				size_type s = ((bytes_sent_ - snt) < BUFSIZE) ? (bytes_sent_ - snt) : BUFSIZE;
				//uart_->write(s, (Uart::block_data_t*)rdf_buffer_ + snt);
				debug_->debug("resending chunk");
				send_contiki(s, (block_data_t*)rdf_buffer_ + snt);
				timer_->sleep(TUPLE_SLEEP);
				snt += s;
			}
			debug_->debug("resend done");
			timer_->set_timer<App, &App::on_uart_timeout>(UART_RESEND_INTERVAL, this, (void*)ato_guard_);
		}


		::uint8_t recv_state_;

		void on_receive_uart(Uart::size_t len, Uart::block_data_t *data) {
		//void on_receive_uart(Uart::size_t l, Uart::block_data_t *data_) {
			//block_data_t data[1024];

			//Base64Codec<Os>::decode(reinterpret_cast<block_data_t*>(data_), data, l);
			//size_type len = l / 2;

			//debug_->debug("RECV(%d)", (int)len);


			if(len >= 1 && data[0] == 'O' && data[1] == 'K') {
				debug_->debug("OK");
				//ato_guard_++;
				//timer_->set_timer<App, &App::read_n3>(SEND_INTERVAL, this, 0);
				expect_answer = false;
			}
			//else if(len >= 1 && data[0] == 'E' && data[1] == 'R' && data[2] == 'R') {
				//debug_->debug("ERR");
				////ato_guard_++;
				////if(!waiting_for_resend) {
					////timer_->set_timer<App, &App::uart_resend>(1000, this, 0); //uart_resend();
				////}
			//}
			else {
				debug_->debug("{{%s}}", (char*)data);
			}
		}
#endif

	// }}}
};

// <general wiselib boilerplate>
// {{{
	
	// Application Entry Point & Definiton of allocator
	Allocator allocator_;
	Allocator& get_allocator() { return allocator_; }
	wiselib::WiselibApplication<Os, App> app;
	void application_main(Os::AppMainParameter& amp) { app.init(amp); }
	
// }}}
// </general wiselib boilerplate>

// vim: set ts=4 sw=4 tw=78 noexpandtab foldmethod=marker foldenable :
